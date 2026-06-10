// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include <glob.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
  gst_.pipeline = nullptr;
  gst_.playbin = nullptr;
  gst_.video_convert = nullptr;
  gst_.video_sink = nullptr;
  gst_.output = nullptr;
  gst_.bus = nullptr;
  gst_.buffer = nullptr;

  uri_ = ParseUri(uri);
  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    DestroyPipeline();
    return;
  }
}

GstVideoPlayer::~GstVideoPlayer() {
#ifdef USE_EGL_IMAGE_DMABUF
  UnrefEGLImage();
#endif  // USE_EGL_IMAGE_DMABUF
  Stop();
  DestroyPipeline();
}

// static
void GstVideoPlayer::GstLibraryLoad() { gst_init(NULL, NULL); }

// static
void GstVideoPlayer::GstLibraryUnload() { gst_deinit(); }

bool GstVideoPlayer::Init() {
  if (!gst_.pipeline) {
    return false;
  }

  if (!Preroll()) {
    DestroyPipeline();
    return false;
  }

  // With sync=TRUE, fakesink only delivers frames once the pipeline clock is
  // running (PLAYING state). Live HLS prerolls with NO_PREROLL so sinks never
  // receive a buffer in PAUSED. Advance to PLAYING now so the V4L2 hardware
  // decoder starts producing frames and HandoffHandler can capture real
  // dimensions before OnNotifyInitialized() is sent to Flutter.
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Init: Failed to reach PLAYING state" << std::endl;
    DestroyPipeline();
    return false;
  }

  // Wait up to 5 s for HandoffHandler to deliver the first decoded frame.
  {
    std::unique_lock<std::mutex> lock(mutex_first_frame_);
    first_frame_cv_.wait_for(lock, std::chrono::seconds(5),
                             [this] { return first_frame_ready_.load(); });
  }

  // Complete init here (platform thread) if a frame arrived during the wait.
  // If we timed out, HandoffHandler will call OnNotifyInitialized() on the
  // first frame it delivers (deferred-init path).
  if (first_frame_ready_.load() && !initialized_.exchange(true)) {
    stream_handler_->OnNotifyInitialized();
    stream_handler_->OnNotifyPlaying(true);
  }

  StartWatchdog();
  return true;
}

bool GstVideoPlayer::Play() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PLAYING" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(true);
  return true;
}

bool GstVideoPlayer::Pause() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::Stop() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to READY" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::SetVolume(double volume) {
  if (!gst_.playbin) {
    return false;
  }

  volume_ = volume;
  g_object_set(gst_.playbin, "volume", volume, NULL);
  return true;
}

bool GstVideoPlayer::SetPlaybackRate(double rate) {
  if (!gst_.playbin) {
    return false;
  }

  if (rate <= 0) {
    std::cerr << "Rate " << rate << " is not supported" << std::endl;
    return false;
  }

  auto position = GetCurrentPosition();
  if (position < 0) {
    return false;
  }

  if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME,
                        GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                        position * GST_MSECOND, GST_SEEK_TYPE_SET,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to set playback rate to " << rate
              << " (gst_element_seek failed)" << std::endl;
    return false;
  }

  playback_rate_ = rate;
  mute_ = (rate < 0.5 || rate > 2);
  g_object_set(gst_.playbin, "mute", mute_, NULL);

  return true;
}

bool GstVideoPlayer::SetSeek(int64_t position) {
  auto nanosecond = position * 1000 * 1000;
  if (!gst_element_seek(
          gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
          GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek " << nanosecond << std::endl;
    return false;
  }
  return true;
}

int64_t GstVideoPlayer::GetDuration() {
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 duration_msec;
  if (!gst_element_query_duration(gst_.pipeline, fmt, &duration_msec)) {
    std::cerr << "Failed to get duration" << std::endl;
    return -1;
  }
  duration_msec /= GST_MSECOND;
  return duration_msec;
}

int64_t GstVideoPlayer::GetCurrentPosition() {
  gint64 position = 0;

  // Sometimes we get an error when playing streaming videos.
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position)) {
    return 0;
  }

  // TODO: We need to handle this code in the proper plase.
  // The VideoPlayer plugin doesn't have a main loop, so EOS message
  // received from GStreamer cannot be processed directly in a callback
  // function. This is because the event channel message of playback complettion
  // needs to be thrown in the main thread.
  {
    std::unique_lock<std::mutex> lock(mutex_event_completed_);
    if (is_completed_) {
      is_completed_ = false;
      lock.unlock();

      if (auto_repeat_) {
        SetSeek(0);
      } else {
        stream_handler_->OnNotifyCompleted();
      }
    }
  }

  return position / GST_MSECOND;
}

#ifdef USE_EGL_IMAGE_DMABUF
void* GstVideoPlayer::GetEGLImage(void* egl_display, void* egl_context) {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  GstMemory* memory = gst_buffer_peek_memory(gst_.buffer, 0);
  if (gst_is_dmabuf_memory(memory)) {
    UnrefEGLImage();

    gint fd = gst_dmabuf_memory_get_fd(memory);
    gst_gl_display_egl_ =
        gst_gl_display_egl_new_with_egl_display(reinterpret_cast<gpointer>(egl_display));
    gst_gl_ctx_ = gst_gl_context_new_wrapped(
        GST_GL_DISPLAY_CAST(gst_gl_display_egl_), reinterpret_cast<guintptr>(egl_context),
        GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);

    gst_gl_context_activate(gst_gl_ctx_, TRUE);

    gst_egl_image_ =
        gst_egl_image_from_dmabuf(gst_gl_ctx_, fd, &gst_video_info_, 0, 0);
    return reinterpret_cast<void*>(gst_egl_image_get_image(gst_egl_image_));
  }
  return nullptr;
}

void GstVideoPlayer::UnrefEGLImage() {
  if (gst_egl_image_) {
    gst_egl_image_unref(gst_egl_image_);
    gst_object_unref(gst_gl_ctx_);
    gst_object_unref(gst_gl_display_egl_);
    gst_egl_image_ = NULL;
    gst_gl_ctx_ = NULL;
    gst_gl_display_egl_ = NULL;
  }
}
#endif  // USE_EGL_IMAGE_DMABUF

const uint8_t* GstVideoPlayer::GetFrameBuffer() {
#ifdef USE_EGL_IMAGE_DMABUF
  // When using DMA-BUF zero-copy, CPU-side framebuffer extraction is not used.
  // Callers should use GetEGLImage instead.
  return nullptr;
#else
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  const uint32_t pixel_bytes = width_ * height_ * 4;
  gst_buffer_extract(gst_.buffer, 0, pixels_.get(), pixel_bytes);
  return reinterpret_cast<const uint8_t*>(pixels_.get());
#endif
}

// Pick an ALSA device for audio output by detecting which HDMI connector is
// physically connected via DRM, then mapping back to the matching ALSA card.
// DRM connector "HDMI-A-N" pairs with ALSA card id "vc4hdmi{N-1}".
// Falls back to plughw:0,0 (3.5mm jack on Pi) if no HDMI is connected.
static std::string PickAudioDevice() {
  for (int hdmi_idx = 1; hdmi_idx <= 4; ++hdmi_idx) {
    char pattern[64];
    std::snprintf(pattern, sizeof(pattern),
                  "/sys/class/drm/card*-HDMI-A-%d/status", hdmi_idx);
    glob_t g{};
    if (glob(pattern, 0, nullptr, &g) != 0) {
      globfree(&g);
      continue;
    }
    bool connected = false;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
      std::ifstream f(g.gl_pathv[i]);
      std::string status;
      if (f && std::getline(f, status) && status == "connected") {
        connected = true;
        break;
      }
    }
    globfree(&g);
    if (!connected) continue;

    char target[16];
    std::snprintf(target, sizeof(target), "vc4hdmi%d", hdmi_idx - 1);
    for (int card = 0; card < 32; ++card) {
      char id_path[64];
      std::snprintf(id_path, sizeof(id_path), "/proc/asound/card%d/id", card);
      std::ifstream f(id_path);
      std::string id;
      if (f && std::getline(f, id) && id == target) {
        char dev[32];
        std::snprintf(dev, sizeof(dev), "plughw:%d,0", card);
        std::cout << "PickAudioDevice: HDMI-A-" << hdmi_idx
                  << " connected, ALSA card " << card << " (" << id
                  << ") -> " << dev << std::endl;
        return dev;
      }
    }
    std::cerr << "PickAudioDevice: HDMI-A-" << hdmi_idx
              << " connected but no ALSA card matching '" << target
              << "' found" << std::endl;
  }
  std::cout << "PickAudioDevice: No connected HDMI, falling back to plughw:0,0"
            << std::endl;
  return "plughw:0,0";
}

static void SourceSetupCallback(GstElement* playbin, GstElement* source,
                                gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  const char* type_name = G_OBJECT_TYPE_NAME(source);
  std::cout << "SOURCE-SETUP: element=" << type_name << std::endl;

  if (g_strcmp0(type_name, "GstCurlHttpSrc") == 0) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(source);

    // timeout = CURLOPT_TIMEOUT: total transfer time per segment request.
    g_object_set(source, "timeout", (gint)30, "compress", TRUE, "keep-alive", TRUE, NULL);

    // connect-timeout = CURLOPT_CONNECTTIMEOUT: not present in all GStreamer builds.
    if (g_object_class_find_property(klass, "connect-timeout")) {
      g_object_set(source, "connect-timeout", (gint)10, NULL);
      std::cout << "SOURCE-SETUP: curlhttpsrc connect-timeout=10s" << std::endl;
    } else {
      std::cout << "SOURCE-SETUP: curlhttpsrc connect-timeout unavailable (old version)" << std::endl;
    }

    // low-speed-limit / low-speed-time: abort if rate stays below 200 B/s for
    // 15 s — catches partial-body hangs that CURLOPT_TIMEOUT alone misses.
    if (g_object_class_find_property(klass, "low-speed-time")) {
      g_object_set(source, "low-speed-time", (gint)15, "low-speed-limit", (glong)200, NULL);
      std::cout << "SOURCE-SETUP: curlhttpsrc low-speed: <200B/s for 15s = abort" << std::endl;
    } else {
      std::cout << "SOURCE-SETUP: curlhttpsrc low-speed unavailable (old version)" << std::endl;
    }

    gint t = 0;
    g_object_get(source, "timeout", &t, NULL);
    std::cout << "SOURCE-SETUP: curlhttpsrc timeout=" << t << "s" << std::endl;

    if (!self->auth_headers_.all_headers.empty()) {
      GstStructure* headers = gst_structure_new_empty("extra-headers");
      for (const auto& [key, value] : self->auth_headers_.all_headers) {
        gst_structure_set(headers, key.c_str(), G_TYPE_STRING, value.c_str(),
                          NULL);
      }
      g_object_set(source, "extra-headers", headers, NULL);
      gst_structure_free(headers);
    }
  } else if (g_strcmp0(type_name, "GstSoupHTTPSrc") == 0) {
    std::cerr << "WARNING: souphttpsrc selected despite rank override" << std::endl;
  }
}

void GstVideoPlayer::SetAuthHeaders(
    const std::map<std::string, std::string>& headers) {
  auth_headers_.all_headers = headers;
}

bool GstVideoPlayer::CreatePipeline() {
  GstRegistry* registry = gst_registry_get();

  // curlhttpsrc handles HTTPS streams reliably; souphttpsrc has TLS issues.
  GstPluginFeature* curl_feature = gst_registry_lookup_feature(registry, "curlhttpsrc");
  GstPluginFeature* soup_feature = gst_registry_lookup_feature(registry, "souphttpsrc");
  if (curl_feature) {
    gst_plugin_feature_set_rank(curl_feature, GST_RANK_PRIMARY + 300);
    gst_object_unref(curl_feature);
  } else {
    std::cerr << "CreatePipeline: WARNING - curlhttpsrc not found" << std::endl;
  }
  if (soup_feature) {
    gst_plugin_feature_set_rank(soup_feature, GST_RANK_NONE);
    gst_object_unref(soup_feature);
  }

  // Prefer hardware H.264 decode on the Pi.
  GstPluginFeature* v4l2_h264 = gst_registry_lookup_feature(registry, "v4l2h264dec");
  if (v4l2_h264) {
    gst_plugin_feature_set_rank(v4l2_h264, GST_RANK_PRIMARY + 300);
    gst_object_unref(v4l2_h264);
  }

  gst_.pipeline = gst_pipeline_new("pipeline");
  if (!gst_.pipeline) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    return false;
  }

  gst_.playbin = gst_element_factory_make("playbin", "playbin");
  if (!gst_.playbin) {
    std::cerr << "Failed to create playbin" << std::endl;
    return false;
  }

  g_signal_connect(gst_.playbin, "source-setup", G_CALLBACK(SourceSetupCallback), this);

  // Video converter: prefer v4l2convert (hardware-accelerated) with RGBA output,
  // fall back to software videoconvert if unavailable.
  gst_.video_convert = gst_element_factory_make("v4l2convert", "videoconvert");
  if (!gst_.video_convert) {
    gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
  }
  if (!gst_.video_convert) {
    std::cerr << "Failed to create videoconvert" << std::endl;
    return false;
  }

  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
  if (!gst_.video_sink) {
    std::cerr << "Failed to create videosink" << std::endl;
    return false;
  }

  GstElement* video_queue = gst_element_factory_make("queue", "vqueue");
  if (!video_queue) {
    std::cerr << "Failed to create video queue" << std::endl;
    return false;
  }
  // Limit the queue size to prevent buffer accumulation after the hardware decoder,
  // which otherwise exhausts the Raspberry Pi's CMA (Contiguous Memory Allocator)
  // and floods the kernel with 'swiotlb buffer is full' errors.
  g_object_set(G_OBJECT(video_queue),
               "max-size-buffers", (guint)5,
               "max-size-bytes",   (guint)0,
               "max-size-time",    (guint64)0,
               NULL);


  gst_.output = gst_bin_new("output");
  if (!gst_.output) {
    std::cerr << "Failed to create output bin" << std::endl;
    return false;
  }

  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  if (!gst_.bus) {
    std::cerr << "Failed to get bus" << std::endl;
    return false;
  }
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

  // sync=TRUE: fakesink paces video frames to the GStreamer clock, keeping
  // A/V in sync. async=TRUE (default): fakesink participates in preroll,
  // which is what we want — Init() waits for the first decoded frame before
  // calling OnNotifyInitialized().
  g_object_set(G_OBJECT(gst_.video_sink), "sync", TRUE, "qos", FALSE, NULL);
  g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect(G_OBJECT(gst_.video_sink), "handoff",
                   G_CALLBACK(HandoffHandler), this);

  // queue -> v4l2convert -> video/x-raw,format=RGBA -> fakesink
  gst_bin_add_many(GST_BIN(gst_.output), video_queue, gst_.video_convert,
                   gst_.video_sink, NULL);
  if (!gst_element_link(video_queue, gst_.video_convert)) {
    std::cerr << "Failed to link queue to videoconvert" << std::endl;
    return false;
  }
  auto* caps = gst_caps_from_string("video/x-raw,format=RGBA");
  auto link_ok = gst_element_link_filtered(gst_.video_convert, gst_.video_sink, caps);
  gst_caps_unref(caps);
  if (!link_ok) {
    std::cerr << "Failed to link videoconvert to fakesink" << std::endl;
    return false;
  }

  auto* sinkpad = gst_element_get_static_pad(video_queue, "sink");
  auto* ghost_sinkpad = gst_ghost_pad_new("sink", sinkpad);
  gst_pad_set_active(ghost_sinkpad, TRUE);
  gst_element_add_pad(gst_.output, ghost_sinkpad);
  gst_object_unref(sinkpad);

  g_object_set(gst_.playbin, "uri", uri_.c_str(), NULL);
  g_object_set(gst_.playbin, "video-sink", gst_.output, NULL);

  const gint GST_PLAY_FLAG_VIDEO        = 0x00000001;
  const gint GST_PLAY_FLAG_AUDIO        = 0x00000002;
  const gint GST_PLAY_FLAG_TEXT         = 0x00000004;
  const gint GST_PLAY_FLAG_NATIVE_VIDEO = 0x00000800;
  const gint GST_PLAY_FLAG_BUFFERING    = 0x00000080;

  gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO |
               GST_PLAY_FLAG_NATIVE_VIDEO | GST_PLAY_FLAG_BUFFERING;
  flags &= ~GST_PLAY_FLAG_TEXT;
  g_object_set(gst_.playbin, "flags", flags, NULL);

  // The origin emits 10 s HLS segments, so keep roughly three segments of
  // time-depth. This gives the player enough cushion for a late segment fetch.
  g_object_set(gst_.playbin,
               "buffer-size",     (gint)10485760,         // 10 MiB
               "buffer-duration", (gint64)30000000000LL,  // 30 s
               "connection-speed", (guint64)5000,
               NULL);

  // Audio: audioconvert -> audioresample -> alsasink (HDMI auto-detected).
  // Explicit alsasink is required because Buildroot omits autoaudiosink.
  // audioresample prevents ALSA sample-rate-mismatch underruns under load.
  GstElement* audio_bin  = gst_bin_new("audio_bin");
  GstElement* conv       = gst_element_factory_make("audioconvert",  "audio_convert");
  GstElement* resample   = gst_element_factory_make("audioresample", "audio_resample");
  GstElement* audio_sink = gst_element_factory_make("alsasink",      "audio_alsa");

  if (!audio_bin || !conv || !resample || !audio_sink) {
    std::cerr << "CreatePipeline: Failed to create audio elements" << std::endl;
  } else {
    std::string audio_device = PickAudioDevice();
    g_object_set(audio_sink, "device", audio_device.c_str(), NULL);
    g_object_set(audio_sink,
                 "latency-time", (gint64)50000,   // 50 ms
                 "buffer-time",  (gint64)100000,  // 100 ms
                 NULL);

    gst_bin_add_many(GST_BIN(audio_bin), conv, resample, audio_sink, NULL);
    if (!gst_element_link(conv, resample) || !gst_element_link(resample, audio_sink)) {
      std::cerr << "CreatePipeline: Failed to link audio chain" << std::endl;
    } else {
      GstPad* apad = gst_element_get_static_pad(conv, "sink");
      GstPad* ghost_apad = gst_ghost_pad_new("sink", apad);
      gst_pad_set_active(ghost_apad, TRUE);
      gst_element_add_pad(audio_bin, ghost_apad);
      gst_object_unref(apad);
      g_object_set(gst_.playbin, "audio-sink", audio_bin, NULL);
    }
  }

  gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.playbin, NULL);

  return true;
}

bool GstVideoPlayer::Preroll() {
  auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Preroll: Failed to change state to PAUSED" << std::endl;
    return false;
  }
  // NO_PREROLL (live source) and ASYNC are both acceptable — not errors.
  // Give GStreamer up to 5 s to settle before Init() advances to PLAYING.
  GstState state;
  gst_element_get_state(gst_.pipeline, &state, NULL, 5 * GST_SECOND);
  return true;
}

void GstVideoPlayer::DestroyPipeline() {
  StopWatchdog();

  if (gst_.video_sink) {
    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  if (gst_.pipeline) {
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
  }

  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }

  if (gst_.bus) {
    gst_object_unref(gst_.bus);
    gst_.bus = nullptr;
  }

  if (gst_.pipeline) {
    gst_object_unref(gst_.pipeline);
    gst_.pipeline = nullptr;
  }

  gst_.playbin = nullptr;
  gst_.output = nullptr;
  gst_.video_sink = nullptr;
  gst_.video_convert = nullptr;
}

std::string GstVideoPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) {
    return uri;
  }

  auto* filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
  if (!filename_uri) {
    std::cerr << "Faild to open " << uri.c_str() << std::endl;
    return uri;
  }
  std::string result_uri(filename_uri);
  g_free(filename_uri);

  return result_uri;
}

void GstVideoPlayer::GetVideoSize(int32_t& width, int32_t& height) {
  if (!gst_.pipeline || !gst_.video_sink) {
    std::cerr
        << "Failed to get video size. The pileline hasn't initialized yet.";
    return;
  }

  auto* sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
  if (!sink_pad) {
    std::cerr << "Failed to get a pad";
    return;
  }

  auto* caps = gst_pad_get_current_caps(sink_pad);
  if (!caps) {
    std::cerr << "Failed to get caps" << std::endl;
    gst_object_unref(sink_pad);
    return;
  }

  auto* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    std::cerr << "Failed to get a structure" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }

  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);

#ifdef USE_EGL_IMAGE_DMABUF
  gboolean res = gst_video_info_from_caps(&gst_video_info_, caps);
  if (!res) {
    std::cerr << "Failed to get a gst_video_info" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }
#endif  // USE_EGL_IMAGE_DMABUF

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
}

// static
void GstVideoPlayer::HandoffHandler(GstElement* fakesink, GstBuffer* buf,
                                    GstPad* new_pad, gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  auto* caps = gst_pad_get_current_caps(new_pad);
  auto* structure = gst_caps_get_structure(caps, 0);

  int width = 0, height = 0;
  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);
  gst_caps_unref(caps);

  {
    std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
    if (width != self->width_ || height != self->height_) {
      self->width_ = width;
      self->height_ = height;
      if (width > 0 && height > 0)
        self->pixels_.reset(new uint32_t[width * height]);
    }
    if (self->gst_.buffer)
      gst_buffer_unref(self->gst_.buffer);
    self->gst_.buffer = gst_buffer_ref(buf);
  }

  // Wake Init()'s condition variable on the very first frame.
  if (!self->first_frame_ready_.exchange(true)) {
    self->first_frame_cv_.notify_all();
  }

  // Deferred-init path: only fires if Init() timed out before this frame
  // arrived and left initialized_ = false. In the normal case Init() wins
  // the exchange and calls OnNotifyInitialized() itself.
  if (!self->initialized_.exchange(true)) {
    self->stream_handler_->OnNotifyInitialized();
    self->stream_handler_->OnNotifyPlaying(true);
  }

  self->stream_handler_->OnNotifyFrameDecoded();
}

void GstVideoPlayer::StartWatchdog() {
  if (watchdog_running_.exchange(true)) return; // already running
  {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    last_buffering_progress_time_ = std::chrono::steady_clock::now();
  }
  watchdog_thread_ = std::thread([this]() {
    constexpr auto kCheckInterval = std::chrono::seconds(10);
    constexpr int kStallTimeoutSecs = 30;

    while (true) {
      std::chrono::steady_clock::time_point progress_snap;
      {
        std::unique_lock<std::mutex> lock(watchdog_mutex_);
        watchdog_cv_.wait_for(lock, kCheckInterval);
        if (!watchdog_running_.load()) break;
        progress_snap = last_buffering_progress_time_;
      }

      int pct = last_buffering_percent_.load();
      if (pct < 0 || pct >= 100) continue; // not currently buffering

      auto now = std::chrono::steady_clock::now();
      auto stalled_secs = std::chrono::duration_cast<std::chrono::seconds>(
          now - progress_snap).count();

      // Only log when genuinely stalled — suppress the noise from notify_one()
      // wakeups during normal buffer fill (stalled_secs == 0 in that case).
      if (stalled_secs > 0) {
        std::cout << "WATCHDOG: buffer=" << pct << "% no-progress-for="
                  << stalled_secs << "s" << std::endl;
      }

      if (stalled_secs >= kStallTimeoutSecs) {
        std::string msg = "Network stall: buffer stuck at " +
                          std::to_string(pct) + "% for " +
                          std::to_string(stalled_secs) + "s";
        std::cout << "WATCHDOG: " << msg << " — notifying Flutter to re-init" << std::endl;
        watchdog_running_.store(false);
        bool expected = false;
        if (error_notified_.compare_exchange_strong(expected, true)) {
          stream_handler_->OnNotifyError(msg);
        }
        break;
      }
    }
  });
}

void GstVideoPlayer::StopWatchdog() {
  watchdog_running_.store(false);
  watchdog_cv_.notify_all();
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
      self->is_completed_ = true;
      break;
    }
    case GST_MESSAGE_BUFFERING: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      gint percent;
      gst_message_parse_buffering(message, &percent);

      if (percent != self->last_buffering_percent_.load()) {
        self->last_buffering_percent_.store(percent);

        // Reset watchdog stall timer on any percent change (= buffer progress).
        {
          std::lock_guard<std::mutex> lock(self->watchdog_mutex_);
          self->last_buffering_progress_time_ = std::chrono::steady_clock::now();
        }
        self->watchdog_cv_.notify_one();

        gint64 position = 0;
        const bool has_position = gst_element_query_position(
            self->gst_.pipeline, GST_FORMAT_TIME, &position);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - self->buffering_log_start_time_);

        // Extended buffering stats: download rate and ETA (mpv-style).
        GstBufferingMode mode;
        gint avg_in_bps, avg_out_bps;
        gint64 buffering_left_ms;
        gst_message_parse_buffering_stats(message, &mode, &avg_in_bps,
                                          &avg_out_bps, &buffering_left_ms);

        std::cout << "BUFFERING: " << percent << "% elapsed="
                  << elapsed.count() << "s";
        if (has_position) {
          std::cout << " pos=" << (position / GST_SECOND) << "s";
        }
        if (avg_in_bps > 0) {
          std::cout << " dl=" << (avg_in_bps / 1024) << "KB/s";
        }
        if (buffering_left_ms > 0) {
          std::cout << " eta=" << (buffering_left_ms / 1000) << "s";
        }
        std::cout << " cache-target=30s/10MiB from "
                  << GST_MESSAGE_SRC_NAME(message) << std::endl;
      }
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      // Log pipeline-level state transitions only (element noise is too verbose).
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->gst_.pipeline)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed(message, &old_s, &new_s, &pending);
        std::cout << "PIPELINE-STATE: "
                  << gst_element_state_get_name(old_s) << " -> "
                  << gst_element_state_get_name(new_s);
        if (pending != GST_STATE_VOID_PENDING) {
          std::cout << " (pending " << gst_element_state_get_name(pending) << ")";
        }
        std::cout << std::endl;
      }
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(message, &error, &debug);
      std::cout << "WARNING from " << GST_OBJECT_NAME(message->src)
                << ": " << (error->message ? error->message : "?");
      if (debug && debug[0]) std::cout << "\n  debug: " << debug;
      std::cout << std::endl;
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      gchar* debug;
      GError* error;
      gst_message_parse_error(message, &error, &debug);
      std::string error_msg = error->message ? error->message : "unknown error";
      std::cout << "ERROR from " << GST_OBJECT_NAME(message->src)
                << ": " << error_msg;
      if (debug && debug[0]) std::cout << "\n  debug: " << debug;
      std::cout << std::endl;
      g_free(debug);
      g_error_free(error);
      // Stop the watchdog then fire OnNotifyError exactly once, even if the
      // watchdog and GST_MESSAGE_ERROR race at the same 30s boundary.
      self->watchdog_running_.store(false);
      self->watchdog_cv_.notify_all();
      bool expected = false;
      if (self->error_notified_.compare_exchange_strong(expected, true)) {
        self->stream_handler_->OnNotifyError(error_msg);
      }
      break;
    }
    default:
      break;
  }
  return GST_BUS_DROP;
}
