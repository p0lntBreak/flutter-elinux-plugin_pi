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
  std::cout << "Init: Starting..." << std::endl;
  
  if (!gst_.pipeline) {
    std::cerr << "Init: Pipeline is null!" << std::endl;
    return false;
  }

  // Prerolls before getting information from the pipeline.
  if (!Preroll()) {
    std::cerr << "Init: Preroll failed!" << std::endl;
    DestroyPipeline();
    return false;
  }


  // Sets internal video size and buffier.
  GetVideoSize(width_, height_);
  std::cout << "Init: Video size: " << width_ << "x" << height_ << std::endl;
  
  pixels_.reset(new uint32_t[width_ * height_]);

  stream_handler_->OnNotifyInitialized();
  
  std::cout << "Init: Completed successfully" << std::endl;

  // TEMPORARY TEST: Auto-play for live streams
  bool is_live = (uri_.find("/live/") != std::string::npos) || 
                 (uri_.find("live.") != std::string::npos) ||
                 (uri_.find("livestream") != std::string::npos);
  
  if (is_live) {
    std::cout << "Init: AUTO-STARTING live stream (TEMPORARY TEST)" << std::endl;
    
    // Force to PLAYING immediately
    GstStateChangeReturn ret = gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING);
    std::cout << "Init: Auto-play result: " << ret << std::endl;
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
      std::cerr << "Init: Auto-play FAILED!" << std::endl;
    } else {
      std::cout << "Init: Auto-play initiated, pipeline should start playing" << std::endl;
      stream_handler_->OnNotifyPlaying(true);
    }
  }

  return true;
}

bool GstVideoPlayer::Play() {
  std::cout << "=== Play() called ===" << std::endl;
  
  GstState current, pending;
  gst_element_get_state(gst_.pipeline, &current, &pending, 0);
  std::cout << "Play: Current state=" << current << ", Pending=" << pending << std::endl;
  
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Play: FAILED to change state to PLAYING" << std::endl;
    return false;
  }

  std::cout << "Play: State change to PLAYING initiated" << std::endl;
  
  // Wait briefly to confirm state change
  GstStateChangeReturn ret = gst_element_get_state(gst_.pipeline, &current, &pending, 2 * GST_SECOND);
  std::cout << "Play: After state change - Current=" << current 
            << ", Pending=" << pending 
            << ", Result=" << ret << std::endl;
  
  if (current == GST_STATE_PLAYING) {
    std::cout << "Play: Successfully reached PLAYING state!" << std::endl;
  } else {
    std::cerr << "Play: WARNING - Not in PLAYING state yet!" << std::endl;
  }

  stream_handler_->OnNotifyPlaying(true);
  return true;
}


bool GstVideoPlayer::Pause() {
  if (!gst_.pipeline) {
    return false;
  }
  
  // For V4L2 decoders, ensure buffers are properly handled during pause
  // Don't flush here as we want to resume playback smoothly
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::Stop() {
  if (!gst_.pipeline) {
    return false;
  }

  // CRITICAL: Flush buffers before stopping to prevent kernel buffer leaks
  // This ensures V4L2 decoder buffers are properly returned to the kernel
  std::cout << "Stop: Flushing pipeline buffers..." << std::endl;
  
  // First, ensure we're paused to stop new buffers from being queued
  GstState current, pending;
  gst_element_get_state(gst_.pipeline, &current, &pending, 0);
  if (current == GST_STATE_PLAYING) {
    gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
    // Wait for pause to complete
    gst_element_get_state(gst_.pipeline, &current, &pending, 1 * GST_SECOND);
  }
  
  // Release any held buffer references first
  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }
  
  // Send FLUSH_START event to flush all buffers in the pipeline
  // This is critical for V4L2 decoders to return buffers to the kernel
  GstEvent* flush_start = gst_event_new_flush_start();
  if (flush_start) {
    gst_element_send_event(gst_.pipeline, flush_start);
  }
  
  // Send FLUSH_STOP event to complete the flush
  // FALSE = don't reset time, we're stopping anyway
  GstEvent* flush_stop = gst_event_new_flush_stop(FALSE);
  if (flush_stop) {
    gst_element_send_event(gst_.pipeline, flush_stop);
  }
  
  // Give time for flush events to propagate and buffers to be released
  // This is especially important for V4L2 decoders which need to return
  // buffers to the kernel's videobuf2 framework
  // Note: g_usleep is available through GLib (included by GStreamer)
  if (flush_start && flush_stop) {
    g_usleep(100000); // 100ms delay to allow buffers to be released
  }
  
  // Now change state to READY, which will stop streaming
  // This should now be safe as buffers have been flushed
  GstStateChangeReturn ret = gst_element_set_state(gst_.pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Stop: Failed to change the state to READY" << std::endl;
    return false;
  }
  
  // Wait for state change to complete to ensure buffers are released
  ret = gst_element_get_state(gst_.pipeline, &current, &pending, 2 * GST_SECOND);
  
  if (current != GST_STATE_READY && pending != GST_STATE_READY) {
    std::cerr << "Stop: WARNING - State change may not have completed (current=" 
              << current << ", pending=" << pending << ", ret=" << ret << ")" << std::endl;
  } else {
    std::cout << "Stop: Successfully stopped and flushed buffers" << std::endl;
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
  if (!gst_.pipeline) {
    return 0;
  }

  GstState state, pending;
  gst_element_get_state(gst_.pipeline, &state, &pending, 0);
  
  // Position is only available when pipeline is PAUSED or PLAYING
  if (state < GST_STATE_PAUSED) {
    return 0;
  }

  gint64 position = 0;

 if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position)) {
    // For LIVE streams, position queries often fail - this is normal
    // Don't spam errors for live streams
    bool is_live = (uri_.find("/live/") != std::string::npos) || 
                   (uri_.find("live.") != std::string::npos) ||
                   (uri_.find("livestream") != std::string::npos);
    
    if (!is_live && state == GST_STATE_PLAYING) {
      // Only log for VOD streams when actually playing
      std::cerr << "Failed to get current position (VOD, state=" << state << ")" << std::endl;
    }
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

// Creats a video pipeline using playbin.
// $ playbin uri=<file> video-sink="videoconvert ! video/x-raw,format=RGBA !
// fakesink"
//UPDATE:

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

// Modified SourceSetupCallback - this gets called for EVERY source element
static void SourceSetupCallback(GstElement* playbin, GstElement* source, gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  
  std::cout << "SourceSetupCallback: Source element created: " 
            << GST_ELEMENT_NAME(source) << " (type: " 
            << G_OBJECT_TYPE_NAME(source) << ")" << std::endl;
  
  // Configure curlhttpsrc for BOTH manifest and segment requests
  if (g_strcmp0(G_OBJECT_TYPE_NAME(source), "GstCurlHttpSrc") == 0) {
    std::cout << "SourceSetupCallback: Configuring curlhttpsrc for segment/manifest fetch" << std::endl;
    
    // Connection settings
    g_object_set(source, 
                 "timeout", 30, 
                 "compress", TRUE, 
                 "keep-alive", TRUE,  // CRITICAL for segment reuse
                 NULL);
    
    // Apply ALL authentication headers if present
    if (!self->auth_headers_.all_headers.empty()) {
      std::cout << "SourceSetupCallback: Applying " << self->auth_headers_.all_headers.size() 
                << " headers to HTTP request" << std::endl;
      
      // Build extra-headers structure with ALL headers
      GstStructure* headers = gst_structure_new_empty("extra-headers");
      
      for (const auto& [key, value] : self->auth_headers_.all_headers) {
        gst_structure_set(headers, key.c_str(), G_TYPE_STRING, value.c_str(), NULL);
        std::cout << "  - Applied: " << key << " = " << value << std::endl;
      }
      
      g_object_set(source, "extra-headers", headers, NULL);
      gst_structure_free(headers);
      
      std::cout << "SourceSetupCallback: All headers applied successfully" << std::endl;
    } else {
      std::cout << "SourceSetupCallback: No auth headers to apply" << std::endl;
    }
  }
  // Fallback warning
  else if (g_strcmp0(G_OBJECT_TYPE_NAME(source), "GstSoupHTTPSrc") == 0) {
    std::cerr << "SourceSetupCallback: WARNING - souphttpsrc was selected despite rank changes!" << std::endl;
  }
}


// Add method to set authentication headers BEFORE creating pipeline
void GstVideoPlayer::SetAuthHeaders(const std::map<std::string, std::string>& headers) {
  auth_headers_.all_headers = headers;
  
  std::cout << "SetAuthHeaders: Stored " << headers.size() << " authentication headers:" << std::endl;
  for (const auto& [key, value] : headers) {
    std::cout << "  - " << key << ": " << value << std::endl;
  }
}

bool GstVideoPlayer::CreatePipeline() {
  std::cout << "CreatePipeline: Starting..." << std::endl;
  std::cout << "CreatePipeline: URI = " << uri_ << std::endl;  
  
  // Extract and store auth headers from URI if present
  // Format: https://domain/path?cookie=xxx&token=yyy
  // Or you should store these separately when creating the player
  // For now, assuming they're passed separately
  
  GstRegistry* registry = gst_registry_get();
  
  // Force curlhttpsrc for HTTPS streams
  GstPluginFeature* curl_feature = gst_registry_lookup_feature(registry, "curlhttpsrc");
  GstPluginFeature* soup_feature = gst_registry_lookup_feature(registry, "souphttpsrc");
  
  if (curl_feature) {
    gst_plugin_feature_set_rank(curl_feature, GST_RANK_PRIMARY + 300);
    gst_object_unref(curl_feature);
    std::cout << "CreatePipeline: curlhttpsrc rank boosted to PRIMARY+300" << std::endl;
  } else {
    std::cerr << "CreatePipeline: WARNING - curlhttpsrc not found!" << std::endl;
  }
  
  if (soup_feature) {
    gst_plugin_feature_set_rank(soup_feature, GST_RANK_NONE);
    gst_object_unref(soup_feature);
    std::cout << "CreatePipeline: souphttpsrc rank set to NONE" << std::endl;
  }
  
  // Hardware decoder ranks (unchanged)
  GstPluginFeature* v4l2_h264 = gst_registry_lookup_feature(registry, "v4l2h264dec");
  if (v4l2_h264) {
    gst_plugin_feature_set_rank(v4l2_h264, GST_RANK_PRIMARY + 300);
    gst_object_unref(v4l2_h264);
    std::cout << "CreatePipeline: v4l2h264dec rank boosted to PRIMARY+300" << std::endl;
  }
  
  // Boost hlsdemux - CRITICAL for live streams
  GstPluginFeature* hls_feature = gst_registry_lookup_feature(registry, "hlsdemux");
  if (hls_feature) {
    gst_plugin_feature_set_rank(hls_feature, GST_RANK_PRIMARY + 300);
    gst_object_unref(hls_feature);
    std::cout << "CreatePipeline: hlsdemux rank boosted to PRIMARY+300" << std::endl;
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
  
  // CRITICAL: Connect source-setup to apply headers to ALL segment requests
  g_signal_connect(gst_.playbin, "source-setup", G_CALLBACK(SourceSetupCallback), this);
  std::cout << "CreatePipeline: Connected source-setup signal" << std::endl;
  
  // Video path: configure differently depending on whether we want CPU copies
  // or DMA-BUF zero-copy into EGL.
#ifdef USE_EGL_IMAGE_DMABUF
  // Zero-copy path: no colorspace conversion, keep native dmabuf-backed frames
  // from v4l2h264dec and pass them directly to fakesink.
  gst_.video_convert = nullptr;
  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
#else
  // CPU path: use v4l2convert/videoconvert and RGBA extraction.
  gst_.video_convert = gst_element_factory_make("v4l2convert", "videoconvert");
  if (!gst_.video_convert) {
    std::cout << "CreatePipeline: Fallback to videoconvert (software)" << std::endl;
    gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
  }
  if (!gst_.video_convert) {
    std::cerr << "Failed to create videoconvert" << std::endl;
    return false;
  }
  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
#endif
  if (!gst_.video_sink) {
    std::cerr << "Failed to create videosink" << std::endl;
    return false;
  }
  
  // Create queue
  GstElement* video_queue = gst_element_factory_make("queue", "vqueue");
  if (!video_queue) {
    std::cerr << "Failed to create video queue" << std::endl;
    return false;
  }
  
  // Detect stream type
  bool is_live_stream = (uri_.find("/live/") != std::string::npos) || 
                        (uri_.find("live.") != std::string::npos) ||
                        (uri_.find("livestream") != std::string::npos);
  
  // Queue settings for VOD only
  if (!is_live_stream) {
    std::cout << "CreatePipeline: VOD stream - balanced buffering" << std::endl;
    g_object_set(video_queue,
                 "max-size-buffers", 3,
                 "max-size-time", (guint64)0,
                 "max-size-bytes", 0,
                 NULL);
  }
  
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

  // Configure fakesink - CRITICAL for live streams
  if (is_live_stream) {
    // For live streams: disable sync to prevent blocking on clock
    g_object_set(G_OBJECT(gst_.video_sink), 
                 "sync", FALSE,      // Don't wait for clock - critical for live!
                 "qos", FALSE,       // No quality-of-service
                 "async", FALSE,     // Don't wait for preroll
                 NULL);
    std::cout << "CreatePipeline: Fakesink configured for LIVE (sync=FALSE)" << std::endl;
  } else {
    // For VOD: enable sync for proper playback speed
    g_object_set(G_OBJECT(gst_.video_sink), 
                 "sync", TRUE, 
                 "qos", FALSE, 
                 NULL);
    std::cout << "CreatePipeline: Fakesink configured for VOD (sync=TRUE)" << std::endl;
  }
  
  g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect(G_OBJECT(gst_.video_sink), "handoff",
                   G_CALLBACK(HandoffHandler), this);
  
  // Build output bin
#ifdef USE_EGL_IMAGE_DMABUF
  // Zero-copy: queue -> fakesink with dmabuf-backed video/x-raw
  gst_bin_add_many(GST_BIN(gst_.output), video_queue, gst_.video_sink, NULL);

  GstCaps* caps = gst_caps_from_string("video/x-raw(memory:DMABuf)");
  gboolean link_ok = gst_element_link_filtered(video_queue, gst_.video_sink, caps);
  gst_caps_unref(caps);
  if (!link_ok) {
    std::cerr << "Failed to link queue to sink with dmabuf caps" << std::endl;
    return false;
  }
#else
  // CPU path: queue -> videoconvert -> fakesink (RGBA)
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
    std::cerr << "Failed to link videoconvert to sink" << std::endl;
    return false;
  }
#endif

  auto* sinkpad = gst_element_get_static_pad(video_queue, "sink");
  auto* ghost_sinkpad = gst_ghost_pad_new("sink", sinkpad);
  gst_pad_set_active(ghost_sinkpad, TRUE);
  gst_element_add_pad(gst_.output, ghost_sinkpad);
  gst_object_unref(sinkpad);

  // Configure playbin
  g_object_set(gst_.playbin, "uri", uri_.c_str(), NULL);
  g_object_set(gst_.playbin, "video-sink", gst_.output, NULL);
  
  // Get current flags
  gint flags;
  g_object_get(gst_.playbin, "flags", &flags, NULL);
  
  // Define flag constants
  const gint GST_PLAY_FLAG_VIDEO           = 0x00000001;
  const gint GST_PLAY_FLAG_AUDIO           = 0x00000002;
  const gint GST_PLAY_FLAG_TEXT            = 0x00000004;
  const gint GST_PLAY_FLAG_NATIVE_VIDEO    = 0x00000800;
  const gint GST_PLAY_FLAG_BUFFERING       = 0x00000080;
  
  // Reset and set base flags
  flags = 0;  // Start fresh
  flags |= GST_PLAY_FLAG_VIDEO;
  flags |= GST_PLAY_FLAG_AUDIO;
  flags |= GST_PLAY_FLAG_NATIVE_VIDEO;
  
  // CRITICAL: For live streams, do NOT set buffering flag
  if (!is_live_stream) {
    // Only enable buffering for VOD
    flags |= GST_PLAY_FLAG_BUFFERING;
    std::cout << "CreatePipeline: Buffering ENABLED for VOD" << std::endl;
  } else {
    std::cout << "CreatePipeline: Buffering DISABLED for LIVE stream" << std::endl;
  }
  
  // Disable text/subtitles
  flags &= ~GST_PLAY_FLAG_TEXT;
  
  g_object_set(gst_.playbin, "flags", flags, NULL);
  std::cout << "CreatePipeline: Playbin flags set to 0x" << std::hex << flags << std::dec << std::endl;
  
  // Connection speed for adaptive streaming
  g_object_set(gst_.playbin, "connection-speed", 5000, NULL);
  
  // Buffer cushion: gives the sync=FALSE decoder something to consume during
  // network jitter so multiqueue doesn't oscillate between 0% and 100%.
  // LIVE uses a small budget to keep glass-to-glass latency low.
  if (is_live_stream) {
    g_object_set(gst_.playbin,
                 "buffer-size", 524288,               // 512 KB
                 "buffer-duration", 1500000000LL,     // 1.5 s
                 NULL);
    std::cout << "CreatePipeline: LIVE - 512KB/1.5s buffer cushion" << std::endl;
  } else {
    g_object_set(gst_.playbin,
                 "buffer-size", 2097152,              // 2 MB
                 "buffer-duration", 3000000000LL,     // 3 s
                 NULL);
    std::cout << "CreatePipeline: VOD - 2MB/3s buffering" << std::endl;
  }
  
  // Audio configuration
  GstElement* audio_bin = gst_bin_new("audio_bin");
  GstElement* conv = gst_element_factory_make("audioconvert", "audio_convert");
  GstElement* resample = gst_element_factory_make("audioresample", "audio_resample");
  GstElement* sink = gst_element_factory_make("alsasink", "audio_alsa");

  if (!audio_bin || !conv || !resample || !sink) {
    std::cerr << "CreatePipeline: Failed to create audio elements" << std::endl;
  } else {
    std::string audio_device = PickAudioDevice();
    g_object_set(sink, "device", audio_device.c_str(), NULL);

    // 50ms/100ms: safe under HLS+decode scheduling jitter on RPi4.
    // The previous 20ms/40ms caused underruns because segment fetches
    // (HTTP+TLS+AES) could preempt the audio thread for longer than 20ms.
    g_object_set(sink,
                 "latency-time", (gint64)50000,    // 50 ms (in µs)
                 "buffer-time",  (gint64)100000,   // 100 ms (in µs)
                 NULL);

    // audioconvert -> audioresample -> alsasink
    // audioresample handles sample rate mismatches in GStreamer so plughw
    // doesn't fall back to software SRC, which glitches under load.
    gst_bin_add_many(GST_BIN(audio_bin), conv, resample, sink, NULL);
    if (!gst_element_link(conv, resample) || !gst_element_link(resample, sink)) {
      std::cerr << "CreatePipeline: Failed to link audio chain" << std::endl;
    } else {
      GstPad* sinkpad = gst_element_get_static_pad(conv, "sink");
      GstPad* ghost = gst_ghost_pad_new("sink", sinkpad);
      gst_pad_set_active(ghost, TRUE);
      gst_element_add_pad(audio_bin, ghost);
      gst_object_unref(sinkpad);

      g_object_set(gst_.playbin, "audio-sink", audio_bin, NULL);
      std::cout << "CreatePipeline: Audio sink configured (audioconvert -> audioresample -> alsasink)" << std::endl;
    }
  }

  std::cout << "CreatePipeline: SUCCESS - Pipeline ready for live HLS" << std::endl;

  gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.playbin, NULL);

  return true;
}



bool GstVideoPlayer::Preroll() {
  std::cout << "Preroll: Starting..." << std::endl;
  
  if (!gst_.playbin) {
    std::cerr << "Preroll: playbin is null!" << std::endl;
    return false;
  }

  auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  std::cout << "Preroll: State change result: " << result << std::endl;

  // Check bus for messages while waiting
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  bool done = false;
  bool success = false;
  
  while (!done) {
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 
        1 * GST_SECOND,  // Check every second
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | 
                         GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_WARNING));
    
    if (msg != NULL) {
      GError* err;
      gchar* debug_info;
      
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
          gst_message_parse_error(msg, &err, &debug_info);
          std::cerr << "Preroll ERROR from " << GST_OBJECT_NAME(msg->src) 
                    << ": " << err->message << std::endl;
          std::cerr << "Debug: " << (debug_info ? debug_info : "none") << std::endl;
          g_clear_error(&err);
          g_free(debug_info);
          done = true;
          success = false;
          break;
          
        case GST_MESSAGE_WARNING:
          gst_message_parse_warning(msg, &err, &debug_info);
          std::cerr << "Preroll WARNING from " << GST_OBJECT_NAME(msg->src) 
                    << ": " << err->message << std::endl;
          g_clear_error(&err);
          g_free(debug_info);
          break;
          
        case GST_MESSAGE_STATE_CHANGED:
          if (GST_MESSAGE_SRC(msg) == GST_OBJECT(gst_.pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            std::cout << "Preroll: Pipeline state changed from " << old_state 
                      << " to " << new_state << " (pending: " << pending_state << ")" << std::endl;
            
            if (new_state == GST_STATE_PAUSED) {
              std::cout << "Preroll: Reached PAUSED state!" << std::endl;
              done = true;
              success = true;
            }
          }
          break;
          
        default:
          break;
      }
      gst_message_unref(msg);
    } else {
      // Timeout - still waiting
      std::cout << "Preroll: Still waiting..." << std::endl;
    }
  }
  
  gst_object_unref(bus);
  
  if (success) {
    std::cout << "Preroll: Completed successfully" << std::endl;
    return true;
  } else {
    std::cerr << "Preroll: Failed!" << std::endl;
    return false;
  }
}

void GstVideoPlayer::DestroyPipeline() {
  if (gst_.video_sink) {
    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  // CRITICAL: Flush buffers before destroying pipeline to prevent kernel buffer leaks
  if (gst_.pipeline) {
    GstState current, pending;
    gst_element_get_state(gst_.pipeline, &current, &pending, 0);
    
    // If pipeline is playing or paused, flush buffers first
    if (current >= GST_STATE_PAUSED) {
      std::cout << "DestroyPipeline: Flushing buffers before destruction..." << std::endl;
      
      // Send flush events to release all buffers
      GstEvent* flush_start = gst_event_new_flush_start();
      if (flush_start) {
        gst_element_send_event(gst_.pipeline, flush_start);
      }
      
      GstEvent* flush_stop = gst_event_new_flush_stop(FALSE);
      if (flush_stop) {
        gst_element_send_event(gst_.pipeline, flush_stop);
      }
      
      // Wait for flush to complete
      g_usleep(100000); // 100ms
    }
    
    // Release any held buffer references
    {
      std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
      if (gst_.buffer) {
        gst_buffer_unref(gst_.buffer);
        gst_.buffer = nullptr;
      }
    }
    
    // Now set to NULL state
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
    
    // Wait for state change to complete
    gst_element_get_state(gst_.pipeline, &current, &pending, 2 * GST_SECOND);
  }

  if (gst_.buffer) {
    gst_buffer_unref(gst_.buffer);
    gst_.buffer = nullptr;
  }

  if (gst_.bus) {
    gst_object_unref(gst_.bus);
    gst_.bus = nullptr;
  }

  if (gst_.pipeline) {
    gst_object_unref(gst_.pipeline);
    gst_.pipeline = nullptr;
  }

  if (gst_.playbin) {
    gst_.playbin = nullptr;
  }

  if (gst_.output) {
    gst_.output = nullptr;
  }

  if (gst_.video_sink) {
    gst_.video_sink = nullptr;
  }

  if (gst_.video_convert) {
    gst_.video_convert = nullptr;
  }
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

  int width;
  int height;
  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);
  gst_caps_unref(caps);
  if (width != self->width_ || height != self->height_) {
    self->width_ = width;
    self->height_ = height;
    self->pixels_.reset(new uint32_t[width * height]);
    std::cout << "Pixel buffer size: width = " << width
              << ", height = " << height << std::endl;
  }

  std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
  if (self->gst_.buffer) {
    gst_buffer_unref(self->gst_.buffer);
    self->gst_.buffer = nullptr;
  }
  self->gst_.buffer = gst_buffer_ref(buf);
  self->stream_handler_->OnNotifyFrameDecoded();
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
      gint percent;
      gst_message_parse_buffering(message, &percent);
      // Only log transitions to 0% and 100% to avoid spamming the bus handler
      // thread with hundreds of intermediate-percent messages per minute.
      if (percent == 0 || percent == 100) {
        std::cout << "BUFFERING: " << percent << "% from "
                  << GST_MESSAGE_SRC_NAME(message) << std::endl;
      }
      // Never pause for live streams — the pipeline keeps playing through
      // low-buffer events; the 512KB cushion absorbs normal jitter.
      break;
    }
      
    case GST_MESSAGE_ELEMENT: {
      const GstStructure *s = gst_message_get_structure(message);
      const gchar *name = gst_structure_get_name(s);
      std::cout << "ELEMENT MESSAGE: " << name << " from " 
                << GST_MESSAGE_SRC_NAME(message) << std::endl;
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(message, &error, &debug);
      g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 error->message);
      g_printerr("Warning details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      gchar* debug;
      GError* error;
      gst_message_parse_error(message, &error, &debug);
      g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 error->message);
      g_printerr("Error details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    default:
      break;
  }

  // Return GST_BUS_PASS so the message is still posted to the default
  // bus handlers/watchers. Previously returning GST_BUS_DROP prevented
  // other consumers (like timed_pop_filtered in Preroll) from ever
  // seeing STATE_CHANGED messages.
  return GST_BUS_PASS;
}
