// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include "logging.h"

#include <glob.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {
constexpr double kBufferTargetSecs = 30.0;
constexpr gint64 kBufferTargetNs = 30000000000LL;

// --- Anti-flap ABR tuning ---
// The "healthy buffer" line, in seconds. A buffer at/above this PROVES the
// current rung is sustainable, so (a) a low bandwidth estimate (this link's
// estimate is very noisy — cv routinely >100%) must NOT drop the rung above it
// (the buffer absorbs the variance — otherwise pure quality pumping), and
// (b) up-switching is only allowed at/above it. Set to 15s to match the code's
// "comfortable" safety zone AND to stay BELOW the real live-edge buffer plateau
// (which can sit ~17s, not the 30s target) — a 20s line was unreachable at the
// live edge, trapping playback on the lowest rung with no way to climb back.
constexpr double kHealthyBufferSecs = 15.0;
// Even below the floor, require a >=20% drop to persist across this many 1 s
// AbrTick cycles before acting, so a single dipped sample can't flap the rung.
constexpr int kSustainedDropTicks = 3;

// Minimum time between a (non-emergency) down-switch and a subsequent
// up-switch. The 11:20:29 -> 11:20:30 log showed a bandwidth-drop and an
// up-switch fire one second apart on a jittery link; the two decisions
// disagreed on direction but the estimate hadn't actually stabilised, and
// hlsdemux ended up re-selecting essentially the same rung. Holding the new
// (lower) rung for a stability window lets the estimate settle before we
// let it climb again.
constexpr int kPostDownDwellSecs = 20;

// Cold-start rung hint (kbps) fed to hlsdemux as "connection-speed". Sized
// so hlsdemux picks 360p on this project's manifest (960 kbps rendition):
// 1500 leaves headroom above 360p's 960 but stays below 480p's 1800. A
// conservative first pick lets ABR climb toward the rung the link actually
// sustains, instead of forcing top-rung and rebuffering on any link that
// can't feed a 720p first segment inside the preroll window (device log
// 2026-07-31 09:37:08-38 showed exactly this: healthy ~4 Mbps average
// throughput, buffer plateau at 5s during 720p first-segment fetch, false
// NETWORK_TOO_SLOW fire).
//
// KNOWN REGRESSION until Scope 1 lands: an up-switch after a low-rung
// cold-start can trip the bcm2835-codec pool geometry bug (S_FMT "Device
// has no supported format" verified on device 2026-07-30 when growing then
// shrinking the pool). On that path the pipeline errors out and Dart's
// existing reconnect flow rebuilds fresh at the last hint. Bounded churn is
// preferable to never-starting; the root fix is on a separate branch.
constexpr guint64 kColdStartConnSpeedKbps = 1500;

// Cold-start preroll target (seconds of buffered content required before Init()
// returns success). Matches kHealthyBufferSecs on purpose: it's the same "this
// rung is sustainable" line the ABR engine uses. Deliberately no wall-clock
// cap — spinner takes as long as needed; only a hard error (HTTP timeout,
// EOS, resource failure) ends the wait early.
constexpr double kColdStartPrerollSecs = 15.0;

// Poll interval for the preroll wait. Fast enough to feel responsive on a good
// link; slow enough not to spam gst_query_new_buffering().
constexpr int kPrerollPollMs = 500;

// Stable machine prefix on the error string handed to Flutter for a stream that
// is unavailable due to entitlement (subscription lapsed / channel de-entitled),
// as opposed to a transient/network error. The Dart side branches on this: it
// must surface a "check your subscription" flow and NOT enter the reconnect
// loop (reconnect just thrashes re-auth against a stream it can no longer play).
constexpr char kStreamUnavailablePrefix[] = "STREAM_UNAVAILABLE: ";

// Stable machine prefix on the error string handed to Flutter when the preroll
// gate makes no meaningful progress for a long window — i.e. the network is
// sustained below the lowest-rung's segment rate and the 15 s cold-start line
// is genuinely unreachable. The Dart side branches on this to render a "check
// your connection" UI instead of leaving the spinner up indefinitely; it does
// NOT count against the reconnect budget the way a transient error does,
// because retrying against the same slow link will keep failing the same way.
constexpr char kNetworkTooSlowPrefix[] = "NETWORK_TOO_SLOW: ";

// Pathological-preroll detector. Fires when total bytes fetched at the http
// source pad grew by less than kPathologicalPrerollMinDeltaBytes over
// kPathologicalPrerollNoProgressSecs of wallclock. Bytes-based instead of
// buffered-seconds-based because a downstream throttle (multiqueue limits,
// decoder backpressure) can freeze the buffered-seconds signal while the
// network is delivering fine — device log 2026-07-31 09:37 saw buffer
// plateau at 5 s while throughput averaged ~4 Mbps, an obvious false
// pathological signal by the old rule. Bytes-in only sees the network.
//
// 500 KB in 25 s = ~160 kbps sustained — well under even the 240p rung's
// 550 kbps. Any working link fetches more than that; only a genuinely dead
// or captive-portal-blocked network trips this floor.
constexpr int kPathologicalPrerollWarmupSecs = 5;
constexpr int kPathologicalPrerollNoProgressSecs = 25;
constexpr uint64_t kPathologicalPrerollMinDeltaBytes = 500 * 1024;

// True when a GStreamer error/warning is an HTTP entitlement failure (401/403/
// 410) rather than a transient/network fault. souphttpsrc reports 401/403 as
// GST_RESOURCE_ERROR_NOT_AUTHORIZED; we also scan the message/debug text so a
// 410 Gone (channel pulled) or a differently-mapped status is still caught.
bool IsHttpUnavailable(GError* error, const gchar* debug) {
  if (error && error->domain == GST_RESOURCE_ERROR &&
      error->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED) {
    return true;
  }
  std::string text;
  if (error && error->message) text += error->message;
  if (debug) {
    text += ' ';
    text += debug;
  }
  for (const char* token : {"401", "403", "410", "Unauthorized", "Forbidden"}) {
    if (text.find(token) != std::string::npos) return true;
  }
  return false;
}
}  // namespace

GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
  video_player_elinux::InitTimestampedLogging();
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
  // Do NOT call Stop() here: the PAUSED->READY transition tries to deactivate
  // the v4l2 buffer pools while gst_.buffer still holds one of their buffers,
  // orphaning the pool and leaking its CMA memory on every teardown.
  // DestroyPipeline() releases the held buffer first, then goes to NULL.
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
  //
  // Preroll audio-mute: because we transition to PLAYING before the preroll
  // gate below is satisfied, the audio branch would otherwise play the first
  // buffered seconds of audio while the spinner is still up (visible mismatch
  // between UI and stream). Drive mute on our custom audio_volume_ element
  // (playbin's own "mute" no-ops on this pipeline — the custom audio bin
  // doesn't implement GstStreamVolume; playsink logs "No volume control
  // found / Volume/mute is not available"). Video is unaffected —
  // HandoffHandler still needs frames during the wait to capture dimensions.
  if (audio_volume_) {
    g_object_set(audio_volume_, "mute", TRUE, NULL);
  }
  play_state_requested_.store(true);
  if (is_live_) {
    std::cout << "Init: live stream stays in PLAYING after preroll" << std::endl;
  }
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Init: Failed to reach PLAYING state" << std::endl;
    play_state_requested_.store(false);
    DestroyPipeline();
    return false;
  }

  // Preroll gate (task #8a — YouTube-pattern cold start): wait for
  // kColdStartPrerollSecs of *real* buffered content before returning to
  // Flutter. The pipeline is technically PLAYING here (fakesink+sync=TRUE
  // needs it so HandoffHandler can capture first-frame dimensions), but Init()
  // stays blocked and the Dart side keeps the spinner up. No wall-clock cap —
  // only a hard error breaks the wait early. Query logic mirrors
  // LogPlaybackHealth() exactly so both agree on "how much is buffered."
  {
    auto last_progress_log = std::chrono::steady_clock::now();
    double last_logged_secs = -1.0;
    // Pathological-preroll tracking. Watches BYTES fetched at the http source
    // pad, not buffered-seconds — downstream throttles (multiqueue limits,
    // decoder backpressure) can cap buffered-seconds long before the network
    // stops delivering (see device log 2026-07-31 09:37). Fires only when
    // fewer than kPathologicalPrerollMinDeltaBytes have arrived over
    // kPathologicalPrerollNoProgressSecs — a floor no working link crosses.
    const auto preroll_start = std::chrono::steady_clock::now();
    auto stall_check_time = preroll_start;
    uint64_t stall_check_bytes = 0;
    bool stall_check_armed = false;
    while (!error_notified_.load()) {
      gint64 position = 0;
      const bool has_position = gst_element_query_position(
          gst_.pipeline, GST_FORMAT_TIME, &position);

      double buffer_health_secs = -1.0;
      bool used_estimate = true;
      GstQuery* query = gst_query_new_buffering(GST_FORMAT_TIME);
      if (query && gst_element_query(gst_.pipeline, query)) {
        GstFormat format = GST_FORMAT_TIME;
        gint64 start = 0;
        gint64 stop = -1;
        gint64 estimated_total = -1;
        gst_query_parse_buffering_range(query, &format, &start, &stop,
                                        &estimated_total);
        if (format == GST_FORMAT_TIME && has_position && stop >= position) {
          buffer_health_secs = static_cast<double>(stop - position) /
                               static_cast<double>(GST_SECOND);
          used_estimate = false;
        }
      }
      if (query) gst_query_unref(query);

      const int pct = last_buffering_percent_.load();
      if (used_estimate && pct >= 0) {
        // Fallback: percent-derived estimate. Also treat pct>=100 as threshold
        // met — some live sources never emit a valid TIME buffering-range but
        // do drive buffering percent to 100 once the playbin queue is full.
        buffer_health_secs =
            (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;
      }

      if (buffer_health_secs >= kColdStartPrerollSecs || pct >= 100) {
        std::cout << "PREROLL_WAIT: threshold met — buffered="
                  << static_cast<int>(buffer_health_secs) << "s pct=" << pct
                  << (used_estimate ? " (est)" : "") << std::endl;
        break;
      }

      const auto now = std::chrono::steady_clock::now();
      const auto since_log = std::chrono::duration_cast<std::chrono::seconds>(
          now - last_progress_log).count();
      // Log/publish every ~1s so Dart has a live pulse and the health log has
      // a trail. Also fire whenever the value moved by >=1s so a rapid climb
      // on a fast link isn't hidden behind the 1s throttle.
      const bool moved =
          buffer_health_secs >= 0.0 &&
          (last_logged_secs < 0.0 ||
           std::abs(buffer_health_secs - last_logged_secs) >= 1.0);
      if (since_log >= 1 || moved) {
        std::cout << "PREROLL_WAIT: buffered="
                  << static_cast<int>(buffer_health_secs >= 0.0
                                          ? buffer_health_secs
                                          : 0.0)
                  << "s/" << static_cast<int>(kColdStartPrerollSecs)
                  << "s pct=" << pct << (used_estimate ? " (est)" : "")
                  << std::endl;
        // Republish buffering percent so the Dart side has a pulse even on
        // sources whose BUFFERING messages are quiet between edges.
        if (pct >= 0) {
          stream_handler_->OnNotifyBufferingUpdate(pct);
        }
        last_progress_log = now;
        last_logged_secs = buffer_health_secs;
      }

      // Pathological-preroll detection. Watches BYTES fetched at the http
      // source pad. After the warmup window, arm a checkpoint. If fewer than
      // kPathologicalPrerollMinDeltaBytes have arrived since the checkpoint
      // within kPathologicalPrerollNoProgressSecs, the network is genuinely
      // dead (captive portal, DNS blackhole, sub-160 kbps sustained). Otherwise
      // advance the checkpoint. Fires NETWORK_TOO_SLOW so Dart can render the
      // check-connection UI.
      const auto secs_since_start = std::chrono::duration_cast<std::chrono::seconds>(
          now - preroll_start).count();
      const uint64_t bytes_now = total_bytes_fetched_.load(std::memory_order_relaxed);
      if (secs_since_start >= kPathologicalPrerollWarmupSecs) {
        if (!stall_check_armed) {
          // Arm the first checkpoint at the end of the warmup window.
          stall_check_time = now;
          stall_check_bytes = bytes_now;
          stall_check_armed = true;
        } else if (bytes_now - stall_check_bytes >=
                   kPathologicalPrerollMinDeltaBytes) {
          // Meaningful bytes since the checkpoint — advance. The link is
          // delivering; any buffering-percent plateau is downstream, not
          // network. Preroll will clear on its own or the frame-arrival
          // watchdog will act after PLAYING.
          stall_check_time = now;
          stall_check_bytes = bytes_now;
        } else {
          const auto stall_secs = std::chrono::duration_cast<std::chrono::seconds>(
              now - stall_check_time).count();
          if (stall_secs >= kPathologicalPrerollNoProgressSecs) {
            const uint64_t bytes_this_window = bytes_now - stall_check_bytes;
            std::string msg = std::string(kNetworkTooSlowPrefix) +
                "preroll stalled — only " +
                std::to_string(bytes_this_window / 1024) +
                "KB fetched in last " + std::to_string(stall_secs) + "s";
            std::cerr << msg << std::endl;
            bool expected = false;
            if (error_notified_.compare_exchange_strong(expected, true)) {
              last_error_ = msg;
              stream_handler_->OnNotifyError(msg);
            }
            break;
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(kPrerollPollMs));
    }
  }

  // Preroll gate satisfied (or exited due to error): restore the caller's
  // requested mute state. On the error path the pipeline is torn down
  // immediately below, so setting mute back here is harmless.
  if (audio_volume_) {
    g_object_set(audio_volume_, "mute", mute_ ? TRUE : FALSE, NULL);
  }

  // Preroll aborted by a fatal bus error (e.g. HTTP 4xx, EOS on VOD manifest,
  // souphttpsrc inactivity timeout). Bail before the first-frame wait so we
  // don't sit another 5s on a dead pipeline.
  if (error_notified_.load()) {
    std::cerr << "Init: pipeline error during preroll — failing Init()"
              << std::endl;
    DestroyPipeline();
    return false;
  }

  // Wait up to 5 s for HandoffHandler to deliver the first decoded frame, or
  // for a fatal bus error (e.g. typefind failure) to arrive first. After the
  // preroll gate above, the buffer is deep so this should almost always be
  // near-instant; kept intact so the deferred-init path still exists for the
  // corner case where a frame slips past the query.
  {
    std::unique_lock<std::mutex> lock(mutex_first_frame_);
    first_frame_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
      return first_frame_ready_.load() || error_notified_.load();
    });
  }

  // A fatal error arrived before any frame did — the pipeline is dead, not
  // just slow. Returning true here previously told the caller Init succeeded
  // while the pipeline sat wedged at READY with zero frames forever; the
  // watchdog can't rescue this either, since its frame-stall check only runs
  // once state==PLAYING, which this path never reaches.
  if (error_notified_.load() && !first_frame_ready_.load()) {
    std::cerr << "Init: pipeline error before first frame — failing Init()"
              << std::endl;
    DestroyPipeline();
    return false;
  }

  // Complete init here (platform thread) if a frame arrived during the wait.
  // If we timed out, HandoffHandler will call OnNotifyInitialized() on the
  // first frame it delivers (deferred-init path).
  if (first_frame_ready_.load() && !initialized_.exchange(true)) {
    stream_handler_->OnNotifyInitialized();
    stream_handler_->OnNotifyPlaying(true);
  }

  StartWatchdog();
  // ABR engine RE-ENABLED on fix8-ABR. It was previously disabled because a
  // down-switch to the 480p rung crashed the HW ISP (S_FMT AB24 @ 854x480
  // fails when the converter's output format is renegotiated mid-stream). That
  // failure is now removed by pinning the ISP output geometry to a fixed
  // 1280x720 in CreatePipeline (see the sink-bin comment), so the ISP simply
  // scales a 480p rung up to 720p and its output S_FMT never changes. With the
  // switch made safe, the engine can once again drop rungs on genuine sustained
  // congestion instead of rebuffering on the top rung.
  StartAbrEngine();
  return true;
}

bool GstVideoPlayer::Play() {
  play_state_requested_.store(true);
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PLAYING" << std::endl;
    play_state_requested_.store(false);
    return false;
  }

  if (is_live_) {
    std::cout << "Play: live stream confirmed as PLAYING" << std::endl;
  }
  stream_handler_->OnNotifyPlaying(true);
  return true;
}

bool GstVideoPlayer::Pause() {
  if (is_live_) {
    std::cout << "Pause ignored for live stream; keeping pipeline PLAYING"
              << std::endl;
    return true;
  }

  play_state_requested_.store(false);
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::Stop() {
  play_state_requested_.store(false);
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

  // No-op when the rate isn't actually changing (the common case: the Flutter
  // side calls setPlaybackSpeed(1.0) on init/resume). Seeking to set the same
  // rate is pointless and, on a LIVE stream, the seek ALWAYS fails
  // ("Failed to set playback rate to 1 (gst_element_seek failed)") — and if the
  // caller treats that failure as fatal it can blank a reconnect instead of
  // recovering. Returning success without seeking avoids both.
  if (rate == playback_rate_) {
    return true;
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

// Diagnostic: log the caps negotiated on either side of the ISP (v4l2convert)
// whenever they change. user_data is a static string tag identifying the pad.
// This reveals what actually happens across an ABR resolution switch: the sink
// pad shows the DECODER output (e.g. 854x480 vs 1280x720), the src pad shows
// what the ISP produces (pinned 1280x720 if the fixed-geometry hold works). If
// the sink changes to 854x480 while the src stays 1280x720, the ISP is being
// asked to upscale — and a cropped/zoomed picture then confirms it is NOT
// recomputing its scale for the new input (the crop bug we need to fix).
static GstPadProbeReturn IspCapsLogProbe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                         gpointer user_data) {
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    gchar* s = caps ? gst_caps_to_string(caps) : nullptr;
    std::cout << "ISP-CAPS[" << reinterpret_cast<const char*>(user_data)
              << "]: " << (s ? s : "(null)") << std::endl;
    if (s) g_free(s);
  }
  return GST_PAD_PROBE_OK;
}

static void SourceSetupCallback(GstElement* playbin, GstElement* source,
                                gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  const char* type_name = G_OBJECT_TYPE_NAME(source);
  std::cout << "SOURCE-SETUP: element=" << type_name << std::endl;

  if (g_strcmp0(type_name, "GstCurlHttpSrc") == 0) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(source);

    // keep-alive=FALSE: do NOT reuse the HTTP connection across segment
    // requests. Device logs (2026-06-22) showed ~2 min of perfect playback
    // (buffer pinned at 100%) followed by a sudden buffer collapse 100%->9%
    // and a 30s frame freeze — the signature of a keep-alive socket that the
    // CDN edge silently recycled/half-closed. This build's curlhttpsrc is an
    // old version with neither low-speed-time nor connect-timeout, so it
    // cannot detect or abort the dead socket; the fetch just hangs until the
    // blunt total timeout. A fresh connection per segment means a recycled
    // socket can never poison the stream, and a single bad fetch fails in
    // isolation so hlsdemux can retry the next segment without a full reconnect.
    //
    // timeout = CURLOPT_TIMEOUT: total transfer time per segment request.
    // Lowered 30s->12s so a hung fetch aborts and hlsdemux retries BEFORE the
    // 30s frame-stall watchdog tears the whole pipeline down. A healthy ~10s
    // 720p segment (~3.4 MB @ 2.75 Mbps) downloads well within 12s.
    g_object_set(source, "timeout", (gint)12, "compress", TRUE, "keep-alive",
                 FALSE, NULL);

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
    GObjectClass* klass = G_OBJECT_GET_CLASS(source);
    std::cout << "SOURCE-SETUP: using souphttpsrc" << std::endl;

    // souphttpsrc's `timeout` is a BLOCKING-I/O (INACTIVITY) timeout: abort a
    // read that produces no data for N seconds. This is exactly what the old
    // curlhttpsrc could NOT do — curl only had a total-transfer cap, so a
    // segment fetch that TCP-connected then delivered nothing hung until the
    // blunt total timeout, draining the multiqueue and starving the decoder
    // (the recurring buffer-LOW source stall). With an inactivity timeout a
    // stalled fetch dies in 5s and hlsdemux retries, while a slow-but-
    // PROGRESSING segment keeps data flowing and is NOT cut.
    // 15s, not 5s: this is an INACTIVITY timeout (abort a read with no data for
    // N seconds). HLS segments here are ~10s, and at the live edge a legitimate
    // wait for the next segment (or a CDN that holds the request open until the
    // segment is ready) can exceed 5s — a 5s timeout aborts that healthy fetch,
    // causing stutter and preventing the buffer from building. 15s is longer
    // than a segment but still well under the 30s frame-stall watchdog, so a
    // genuinely hung fetch still aborts+retries before a reconnect.
    g_object_set(source, "timeout", (guint)15, NULL);

    // TLS: this CDN previously failed soup's strict certificate check, which is
    // why the project switched to curl. Relax strict verification so soup can
    // connect (the CDN is known/trusted). If soup STILL cannot connect after
    // this, the TLS *backend* (glib-networking) is missing from the rootfs — an
    // image fix, and we revert to curl.
    if (g_object_class_find_property(klass, "ssl-strict")) {
      g_object_set(source, "ssl-strict", FALSE, NULL);
      std::cout << "SOURCE-SETUP: souphttpsrc ssl-strict=FALSE" << std::endl;
    }
    // Retry an aborted/failed fetch a few times before erroring upstream.
    if (g_object_class_find_property(klass, "retries")) {
      g_object_set(source, "retries", (gint)3, NULL);
    }
    // Fresh connection per request — avoid a CDN-recycled half-open socket
    // (same rationale as curl keep-alive=FALSE).
    if (g_object_class_find_property(klass, "keep-alive")) {
      g_object_set(source, "keep-alive", FALSE, NULL);
    }
    if (g_object_class_find_property(klass, "compress")) {
      g_object_set(source, "compress", TRUE, NULL);
    }

    guint st = 0;
    g_object_get(source, "timeout", &st, NULL);
    std::cout << "SOURCE-SETUP: souphttpsrc timeout=" << st
              << "s (inactivity/blocking-IO)" << std::endl;

    if (!self->auth_headers_.all_headers.empty()) {
      GstStructure* headers = gst_structure_new_empty("extra-headers");
      for (const auto& [key, value] : self->auth_headers_.all_headers) {
        gst_structure_set(headers, key.c_str(), G_TYPE_STRING, value.c_str(),
                          NULL);
      }
      g_object_set(source, "extra-headers", headers, NULL);
      gst_structure_free(headers);
    }
  }
}

void GstVideoPlayer::SetAuthHeaders(
    const std::map<std::string, std::string>& headers) {
  auth_headers_.all_headers = headers;
}

bool GstVideoPlayer::CreatePipeline() {
  GstRegistry* registry = gst_registry_get();

  // PREFER souphttpsrc: its `timeout` is a blocking-I/O (inactivity) timeout
  // that aborts a stalled-body fetch (TCP-connected but delivering no data) in
  // seconds, which the old curlhttpsrc cannot do (it only had a blunt total
  // transfer cap, so a stalled fetch hung long enough to drain the buffer and
  // starve the decoder = the recurring source stall). soup's earlier TLS
  // failure is worked around with ssl-strict=FALSE in SourceSetupCallback.
  // curl is kept as a LOWER-ranked fallback in case soup is unavailable.
  GstPluginFeature* curl_feature = gst_registry_lookup_feature(registry, "curlhttpsrc");
  GstPluginFeature* soup_feature = gst_registry_lookup_feature(registry, "souphttpsrc");
  if (soup_feature) {
    gst_plugin_feature_set_rank(soup_feature, GST_RANK_PRIMARY + 300);
    gst_object_unref(soup_feature);
  } else {
    std::cerr << "CreatePipeline: WARNING - souphttpsrc not found" << std::endl;
  }
  if (curl_feature) {
    gst_plugin_feature_set_rank(curl_feature, GST_RANK_PRIMARY + 200);
    gst_object_unref(curl_feature);
  } else {
    std::cerr << "CreatePipeline: WARNING - curlhttpsrc not found" << std::endl;
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

  // ABR engine: catch hlsdemux as soon as decodebin creates it so the
  // throughput probe and connection-speed steering can attach to it.
  g_signal_connect(gst_.pipeline, "deep-element-added",
                   G_CALLBACK(DeepElementAddedHandler), this);

  // Video converter: prefer v4l2convert (hardware-accelerated) with RGBA output,
  // fall back to software videoconvert if unavailable. The HW ISP does BOTH
  // colour conversion (NV12->RGBA) and scaling, which is what lets us pin a
  // fixed output geometry below and survive ABR rendition switches.
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

  // queue -> v4l2convert -> video/x-raw,format=RGBA,1280x720 -> fakesink
  //
  // SEAMLESS ABR SWITCHING (fix8-ABR): pin the converter's OUTPUT geometry to a
  // FIXED 1280x720 RGBA and let the HW ISP scale whatever the decoder produces
  // up/down to it. This mirrors how phones/TVs/browsers switch renditions
  // without a hitch: the display surface is a constant size and the scaler
  // absorbs the input resolution change.
  //
  // Why this fixes the crash: previously the output caps constrained only the
  // format (RGBA), so the output resolution TRACKED the input. An ABR
  // down-switch (720p->480p) therefore changed the ISP's OUTPUT format
  // mid-stream, forcing a VIDIOC_S_FMT for AB24 @ 854x480 that a V4L2 M2M
  // device rejects while streaming (EINVAL) -> pipeline error -> reconnect
  // churn -> crash. With a fixed output geometry the output S_FMT is programmed
  // ONCE at preroll and never renegotiated; only the ISP's INPUT (capture-side
  // NV12) changes on a switch, which the decoder drives via the standard V4L2
  // source-change flow. Bonus: HandoffHandler's width_/height_ now never change
  // after preroll, so the Flutter pixel buffer is allocated once (no per-switch
  // texture resize). Both rungs are 16:9 (854x480 and 1280x720) so upscaling
  // introduces no aspect distortion.
  //
  // NOTE (verify on build host): this assumes the Pi's v4l2convert can scale to
  // a fixed output. It does (the ISP is a scaler). If a future build's HW
  // convert cannot scale, this filtered link will fail negotiation and Init()
  // returns false -> would then need a HW-scale-capable element or an SW
  // videoscale (the SW colour-convert path was already proven too slow at 720p).
  gst_bin_add_many(GST_BIN(gst_.output), video_queue, gst_.video_convert,
                   gst_.video_sink, NULL);
  if (!gst_element_link(video_queue, gst_.video_convert)) {
    std::cerr << "Failed to link queue to videoconvert" << std::endl;
    return false;
  }
  auto* caps = gst_caps_from_string(
      "video/x-raw,format=RGBA,width=1280,height=720");
  auto link_ok = gst_element_link_filtered(gst_.video_convert, gst_.video_sink, caps);
  gst_caps_unref(caps);
  if (!link_ok) {
    std::cerr << "Failed to link videoconvert to fakesink" << std::endl;
    return false;
  }

  // Diagnostic (fix8-ABR): watch the caps on both sides of the ISP so a device
  // run reveals exactly what it negotiates across a 480<->720 rendition switch
  // (see IspCapsLogProbe). Cheap — fires only on a caps change, not per frame.
  if (auto* isp_sink = gst_element_get_static_pad(gst_.video_convert, "sink")) {
    gst_pad_add_probe(isp_sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      IspCapsLogProbe, (gpointer) "convert-sink", NULL);
    gst_object_unref(isp_sink);
  }
  if (auto* isp_src = gst_element_get_static_pad(gst_.video_convert, "src")) {
    gst_pad_add_probe(isp_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      IspCapsLogProbe, (gpointer) "convert-src", NULL);
    gst_object_unref(isp_src);
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
  //
  // STARTUP rung: a high initial connection-speed makes hlsdemux pick the top
  // variant for the first segments, before the ABR engine has its >=2 samples
  // (AbrTick early-returns until then). This is NOT for speed — it's the
  // invariant that keeps the bcm2835-codec V4L2 input pool safe. Starting at a
  // lower rung and letting ABR climb sets the pool to a small geometry first,
  // then GROWS it on up-switch (OK), then tries to SHRINK it on the next
  // down-switch (FAILS: S_FMT "Device has no supported format", verified
  // 2026-07-30). Top-rung startup means the pool is set once at the largest
  // geometry that will ever be seen; every subsequent switch is a no-op or a
  // shrink the ISP can absorb via the pinned 1280x720 output geometry (see
  // the sink-bin comment). If the ABR engine never gets samples (e.g. probe
  // yields nothing) hlsdemux simply stays on the top rung.
  g_object_set(gst_.playbin,
               "buffer-size",     (gint)10485760,         // 10 MiB
               "buffer-duration", kBufferTargetNs,        // 30 s
               "connection-speed", kColdStartConnSpeedKbps,
               NULL);

  // Audio: audioconvert -> audioresample -> volume -> alsasink
  // (HDMI auto-detected). Explicit alsasink is required because Buildroot
  // omits autoaudiosink. audioresample prevents ALSA sample-rate-mismatch
  // underruns under load. The "volume" element is inserted before the sink
  // so that Init() can mute audio during the preroll gate: playbin's own
  // "mute" property forwards to the audio sink's GstStreamVolume interface,
  // which our alsasink-in-a-bin does not implement (playsink logs
  // "No volume control found / Volume/mute is not available" on start),
  // so driving mute on this element is the only mute path that actually
  // silences audio.
  GstElement* audio_bin  = gst_bin_new("audio_bin");
  GstElement* conv       = gst_element_factory_make("audioconvert",  "audio_convert");
  GstElement* resample   = gst_element_factory_make("audioresample", "audio_resample");
  GstElement* volume     = gst_element_factory_make("volume",        "audio_volume");
  GstElement* audio_sink = gst_element_factory_make("alsasink",      "audio_alsa");

  if (!audio_bin || !conv || !resample || !volume || !audio_sink) {
    std::cerr << "CreatePipeline: Failed to create audio elements" << std::endl;
  } else {
    std::string audio_device = PickAudioDevice();
    g_object_set(audio_sink, "device", audio_device.c_str(), NULL);
    // ALSA cushion sizing. Previously 50 ms / 100 ms — tight enough on a Pi 4
    // under Flutter render + V4L2 decode + ISP scale + Wi-Fi soft-IRQ load
    // that a ~100 ms scheduling gap caused alsasink underruns, and because
    // the audio sink provides the pipeline clock under sync=TRUE, each
    // underrun hiccuped the clock: video branch either burst-dropped frames
    // to catch up or held frames to wait, showing visible stutter (device
    // log 2026-07-30 15:48:38-15:48:45 showed exactly this — burst deltas
    // of +80/+68/+38/+35/+17 frames in successive wallclock seconds with a
    // full buffer and 5 Mbps throughput). 100 ms / 500 ms is the standard
    // robust default for Pi-class devices; adds ~400 ms audio-branch latency
    // which is imperceptible for live TV (no interactivity beyond channel
    // changes, which rebuild the pipeline anyway).
    g_object_set(audio_sink,
                 "latency-time", (gint64)100000,   // 100 ms
                 "buffer-time",  (gint64)500000,   // 500 ms
                 NULL);

    gst_bin_add_many(GST_BIN(audio_bin), conv, resample, volume, audio_sink, NULL);
    if (!gst_element_link(conv, resample) ||
        !gst_element_link(resample, volume) ||
        !gst_element_link(volume, audio_sink)) {
      std::cerr << "CreatePipeline: Failed to link audio chain" << std::endl;
    } else {
      GstPad* apad = gst_element_get_static_pad(conv, "sink");
      GstPad* ghost_apad = gst_ghost_pad_new("sink", apad);
      gst_pad_set_active(ghost_apad, TRUE);
      gst_element_add_pad(audio_bin, ghost_apad);
      gst_object_unref(apad);
      g_object_set(gst_.playbin, "audio-sink", audio_bin, NULL);
      audio_volume_ = volume;  // ref held via bin ownership; cleared in DestroyPipeline
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
  // NO_PREROLL is GStreamer's authoritative "this source is LIVE" signal — far
  // more reliable than a duration/seekable heuristic (a live HLS playlist still
  // reports a small sliding-window duration, which looks like VOD). Capture it
  // so EOS handling never treats a live stream as "completed".
  if (result == GST_STATE_CHANGE_NO_PREROLL) {
    is_live_ = true;
    std::cout << "PREROLL: live source (NO_PREROLL) — EOS will not complete/seek"
              << std::endl;
  }
  // Give GStreamer up to 5 s to settle before Init() advances to PLAYING.
  GstState state;
  gst_element_get_state(gst_.pipeline, &state, NULL, 5 * GST_SECOND);
  return true;
}

void GstVideoPlayer::DestroyPipeline() {
  play_state_requested_.store(false);
  StopWatchdog();
  StopAbrEngine();

  if (gst_.video_sink) {
    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  // Release the held frame BEFORE the NULL transition. The buffer belongs to
  // v4l2convert's CMA-backed pool; a pool with an outstanding buffer cannot
  // be deactivated and is orphaned, leaking its V4L2/CMA memory. Repeated
  // reconnect cycles then exhaust CMA ("Failed to allocate required memory",
  // "export failed") and every new pipeline renders a blank screen.
  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }

  if (gst_.pipeline) {
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
  }

  // A handoff in flight between disabling signal-handoffs and the NULL
  // transition may have stored one more frame — release it as well.
  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }

  if (gst_.bus) {
    gst_bus_set_sync_handler(gst_.bus, NULL, NULL, NULL);
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
  audio_volume_ = nullptr;

  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    if (hls_demux_) {
      gst_object_unref(hls_demux_);
      hls_demux_ = nullptr;
    }
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

  // Ground-truth liveness signal for the watchdog: only moves when a real
  // video buffer reaches the sink, so a wedged decoder stalls it.
  self->frames_handed_off_.fetch_add(1, std::memory_order_relaxed);

  self->stream_handler_->OnNotifyFrameDecoded();
}


void GstVideoPlayer::LogPlaybackHealth(
    GstState state, uint64_t frames, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_frame_time) {
  if (!gst_.pipeline) return;

  // Example:
  // Current Position: 120 sec
  // Buffered Until: 140 sec
  // Buffer Health: 20 sec
  // Network Throughput: 3500 kbps
  // Actual measured throughput: throughput = segment_size_bytes / download_time_seconds

  gint64 position = 0;
  const bool has_position = gst_element_query_position(
      gst_.pipeline, GST_FORMAT_TIME, &position);
  const double position_secs = has_position
      ? static_cast<double>(position) / static_cast<double>(GST_SECOND)
      : -1.0;

  const int pct = last_buffering_percent_.load();
  double buffered_until_secs = -1.0;
  double buffer_health_secs = -1.0;
  bool used_estimate = true;

  GstQuery* query = gst_query_new_buffering(GST_FORMAT_TIME);
  if (query && gst_element_query(gst_.pipeline, query)) {
    GstFormat format = GST_FORMAT_TIME;
    gint64 start = 0;
    gint64 stop = -1;
    gint64 estimated_total = -1;
    gst_query_parse_buffering_range(query, &format, &start, &stop,
                                    &estimated_total);
    if (format == GST_FORMAT_TIME && has_position && stop >= position) {
      buffered_until_secs = static_cast<double>(stop) /
                            static_cast<double>(GST_SECOND);
      buffer_health_secs = static_cast<double>(stop - position) /
                           static_cast<double>(GST_SECOND);
      used_estimate = false;
    }
  }
  if (query) gst_query_unref(query);

  if (used_estimate && has_position && pct >= 0) {
    buffer_health_secs = (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;
    buffered_until_secs = position_secs + buffer_health_secs;
  }

  guint64 last_segment_bytes = 0;
  double last_segment_secs = 0.0;
  double last_segment_bps = 0.0;
  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    last_segment_bytes = last_segment_bytes_;
    last_segment_secs = last_segment_download_secs_;
    last_segment_bps = last_segment_throughput_bps_;
  }

  const auto no_frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_frame_time).count();

  std::cout << "HEALTH: state=" << gst_element_state_get_name(state)
            << " Current Position: ";
  if (has_position) {
    std::cout << static_cast<int>(position_secs) << " sec";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffered Until: ";
  if (buffered_until_secs >= 0.0) {
    std::cout << static_cast<int>(buffered_until_secs) << " sec";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffer Health: ";
  if (buffer_health_secs >= 0.0) {
    std::cout << static_cast<int>(buffer_health_secs) << " sec";
    if (used_estimate) std::cout << " (est)";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffer Percent: ";
  if (pct >= 0) {
    std::cout << pct << "%";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Target Buffer: " << static_cast<int>(kBufferTargetSecs)
            << " sec Frames: " << frames
            << " No Frame For: " << no_frame_ms << " ms";

  if (last_segment_bytes > 0 && last_segment_secs > 0.0) {
    std::cout << " Network Throughput: "
              << static_cast<int>(last_segment_bps / 1000.0) << " kbps"
              << " (segment=" << last_segment_bytes << "B/"
              << last_segment_secs << "s)";
  } else {
    std::cout << " Network Throughput: unknown";
  }

  std::cout << std::endl;
}

void GstVideoPlayer::StartWatchdog() {
  if (watchdog_running_.exchange(true)) return; // already running
  {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    last_buffering_progress_time_ = std::chrono::steady_clock::now();
  }
  watchdog_thread_ = std::thread([this]() {
    constexpr auto kCheckInterval = std::chrono::seconds(1);
    constexpr int kFrameStallTimeoutSecs = 30;  // PLAYING but no frame this long

    // Frame-arrival baseline. Locals: touched only by this thread.
    uint64_t last_seen_frames = frames_handed_off_.load(std::memory_order_relaxed);
    auto last_frame_advance_time = std::chrono::steady_clock::now();

    while (true) {
      std::chrono::steady_clock::time_point progress_snap;
      {
        std::unique_lock<std::mutex> lock(watchdog_mutex_);
        watchdog_cv_.wait_for(lock, kCheckInterval);
        if (!watchdog_running_.load()) break;
        progress_snap = last_buffering_progress_time_;
      }

      auto now = std::chrono::steady_clock::now();
      const int pct = last_buffering_percent_.load();

      // --- Check 1 (DIAGNOSTIC ONLY — no longer reconnects): buffer-% plateau.
      // A LIVE stream sitting at the live edge legitimately plateaus below 100%:
      // the cache-target (e.g. 30s) is unsatisfiable because no segments beyond
      // the live edge exist yet, so buffering messages just stop at ~91% with no
      // % change. That is NOT a stall while frames keep arriving. Firing a full
      // re-init here tore down healthy streams — device log 2026-06-22 showed 7
      // reconnects, ALL buffer-% fires at 91-96%, with ZERO frame-arrival stalls
      // (grep -c "no video frame" = 0). Genuine starvation that actually freezes
      // playback is caught by the frame-arrival check below (Check 2), which is
      // the ground truth. So: log and FALL THROUGH — do not break, do not
      // re-baseline Check 2 (a live-edge plateau keeps the pipeline PLAYING and
      // frames flowing, so Check 2 stays silent on its own). ---
      if (pct >= 0 && pct < 100 && play_state_requested_.load()) {
        auto stalled_secs = std::chrono::duration_cast<std::chrono::seconds>(
            now - progress_snap).count();
        if (stalled_secs >= kFrameStallTimeoutSecs) {
          std::cout << "WATCHDOG: buffer=" << pct << "% no %-change for "
                    << stalled_secs << "s (diagnostic; not fatal — frame-arrival "
                       "governs reconnect)" << std::endl;
        }
      }

      // --- Check 2: Playback frozen (pipeline is PLAYING but no video frame advances) ---
      if (!gst_.pipeline) continue;  // safe: StopWatchdog joins before teardown
      GstState state = GST_STATE_VOID_PENDING;
      gst_element_get_state(gst_.pipeline, &state, nullptr, 0);
      uint64_t frames = frames_handed_off_.load(std::memory_order_relaxed);
      if (state != GST_STATE_PLAYING) {
        // Paused / seeking / not playing — re-baseline so a legitimate pause
        // doesn't look like a freeze when playback resumes.
        last_seen_frames = frames;
        last_frame_advance_time = now;
        LogPlaybackHealth(state, frames, now, last_frame_advance_time);
        continue;
      }

      if (frames != last_seen_frames) {
        last_seen_frames = frames;
        last_frame_advance_time = now;
        LogPlaybackHealth(state, frames, now, last_frame_advance_time);
        continue;
      }

      LogPlaybackHealth(state, frames, now, last_frame_advance_time);
      auto frozen_secs = std::chrono::duration_cast<std::chrono::seconds>(
          now - last_frame_advance_time).count();
      if (frozen_secs > 0) {
        std::cout << "WATCHDOG: PLAYING but no video frame for " << frozen_secs
                  << "s" << std::endl;
      }
      if (frozen_secs >= kFrameStallTimeoutSecs) {
        std::string msg = "Playback frozen: no video frame for " +
                          std::to_string(frozen_secs) + "s while PLAYING";
        std::cout << "WATCHDOG: " << msg << " — notifying Flutter to re-init"
                  << std::endl;
        watchdog_running_.store(false);
        bool expected = false;
        if (error_notified_.compare_exchange_strong(expected, true)) {
          last_error_ = msg;
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

// ============================================================
// ABR engine — Continuous Playback Intelligence
//
// Measurement : pad probe on hlsdemux sink counts segment-download bursts
//               (bytes / active transfer time = true network throughput)
// Prediction  : min(fast mean, harmonic mean) with a jitter discount
// Policy      : buffer-health safety zones; down-switch immediately,
//               up-switch only with headroom + dwell time (anti-flap)
// Actuation   : hlsdemux "connection-speed" (kbps) — the demuxer picks the
//               best rendition under that figure at each segment boundary,
//               so switches never interrupt playback
// ============================================================

namespace {
// connection-speed is guint (kbps) on adaptivedemux-based hlsdemux but
// guint64 on playbin — set it through GValue so the type always matches.
void SetConnectionSpeedKbps(GstElement* element, guint64 kbps) {
  GParamSpec* pspec = g_object_class_find_property(
      G_OBJECT_GET_CLASS(element), "connection-speed");
  if (!pspec) return;
  GValue v = G_VALUE_INIT;
  g_value_init(&v, pspec->value_type);
  if (pspec->value_type == G_TYPE_UINT) {
    g_value_set_uint(&v, static_cast<guint>(kbps));
  } else if (pspec->value_type == G_TYPE_UINT64) {
    g_value_set_uint64(&v, kbps);
  } else {
    g_value_unset(&v);
    return;
  }
  g_object_set_property(G_OBJECT(element), "connection-speed", &v);
  g_value_unset(&v);
}
}  // namespace

// static
void GstVideoPlayer::DeepElementAddedHandler(GstBin* /*bin*/,
                                             GstBin* /*sub_bin*/,
                                             GstElement* element,
                                             gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  gchar* name = gst_element_get_name(element);
  if (name && g_str_has_prefix(name, "hlsdemux")) {
    // Keep a ref for the ABR engine (connection-speed actuation). The throughput
    // probe does NOT go here: hlsdemux's sink pad only carries the periodic
    // manifest/playlist (a few KB) — the media segments are fetched by the
    // demux's internal source and never cross this pad, so a probe here never
    // measures a real segment (throughput read as "unknown").
    {
      std::lock_guard<std::mutex> lock(self->abr_mutex_);
      if (self->hls_demux_) gst_object_unref(self->hls_demux_);
      self->hls_demux_ = GST_ELEMENT(gst_object_ref(element));
    }
    std::cout << "ABR: found hlsdemux element: " << name << std::endl;
  } else if (name && (g_str_has_prefix(name, "souphttpsrc") ||
                      g_str_has_prefix(name, "curlhttpsrc"))) {
    // Segments DO cross the http source's src pad — probe here for real
    // per-segment throughput (size / download-time).
    GstPad* srcpad = gst_element_get_static_pad(element, "src");
    if (srcpad) {
      gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
                        AbrThroughputProbe, self, NULL);
      gst_object_unref(srcpad);
      std::cout << "ABR: attached throughput probe to source: " << name
                << std::endl;
    }
  } else if (name && g_str_has_prefix(name, "multiqueue")) {
    // hlsdemux's downstream multiqueue defaults to max-size-buffers=5,
    // max-size-bytes=2MB, max-size-time=2s — a much smaller window than
    // our 30 s buffering target. On a healthy link the buffering percent
    // plateaus in the mid-teens because the multiqueue is applying
    // backpressure to hlsdemux before hlsdemux has fetched a full 30 s
    // cushion (device log 2026-07-31 09:37 saw pct stuck at ~17 for 25 s
    // on a >4 Mbps average link). Widen the limits to 60 s / 20 MB and
    // remove the buffer-count cap so time/bytes govern instead.
    g_object_set(G_OBJECT(element),
                 "max-size-buffers", (guint)0,          // unlimited count
                 "max-size-bytes",   (guint)(20 * 1024 * 1024),
                 "max-size-time",    (guint64)(60 * GST_SECOND),
                 NULL);
    std::cout << "MULTIQUEUE: widened limits on " << name
              << " (time=60s bytes=20MB buffers=unlimited)" << std::endl;
  }
  g_free(name);
}

// static
GstPadProbeReturn GstVideoPlayer::AbrThroughputProbe(GstPad* /*pad*/,
                                                     GstPadProbeInfo* info,
                                                     gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  const gsize size = gst_buffer_get_size(buf);
  const auto now = std::chrono::steady_clock::now();

  // Bump the monotonic total FIRST, outside the mutex. The preroll gate reads
  // this without contending for abr_mutex_ so it stays responsive even while
  // the streaming thread is mid-burst-close.
  self->total_bytes_fetched_.fetch_add(size, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(self->abr_mutex_);
  // An idle gap separates one segment download from the next.
  if (self->burst_bytes_ > 0 &&
      now - self->burst_last_rx_ > std::chrono::milliseconds(250)) {
    self->CloseBurstLocked();
  }
  if (self->burst_bytes_ == 0) self->burst_start_ = now;
  self->burst_bytes_ += size;
  self->burst_last_rx_ = now;
  return GST_PAD_PROBE_OK;
}

// Caller holds abr_mutex_.
void GstVideoPlayer::CloseBurstLocked() {
  const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                          burst_last_rx_ - burst_start_)
                          .count();
  // Skip playlist refreshes and degenerate timings — only segment-sized
  // transfers measured over a meaningful window are useful samples.
  if (burst_bytes_ >= 64 * 1024 && secs > 0.05) {
    const double throughput_bps = static_cast<double>(burst_bytes_) * 8.0 / secs;
    last_segment_bytes_ = burst_bytes_;
    last_segment_download_secs_ = secs;
    last_segment_throughput_bps_ = throughput_bps;
    abr_samples_.push_back(throughput_bps);
    while (abr_samples_.size() > 16) abr_samples_.pop_front();

    std::cout << "NETWORK: Actual measured throughput: throughput = "
              << burst_bytes_ << " bytes / " << secs << " seconds"
              << " = "
              << static_cast<int>((static_cast<double>(burst_bytes_) / secs) / 1024.0)
              << " KB/s (" << static_cast<int>(throughput_bps / 1000.0)
              << " kbps)" << std::endl;
  }
  burst_bytes_ = 0;
}

void GstVideoPlayer::StartAbrEngine() {
  if (abr_running_.exchange(true)) return;
  last_upswitch_time_ = std::chrono::steady_clock::now();
  abr_thread_ = std::thread([this]() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(abr_mutex_);
        abr_cv_.wait_for(lock, std::chrono::seconds(1));
      }
      if (!abr_running_.load()) break;
      AbrTick();
    }
  });
}

void GstVideoPlayer::StopAbrEngine() {
  abr_running_.store(false);
  abr_cv_.notify_all();
  if (abr_thread_.joinable()) {
    abr_thread_.join();
  }
}

void GstVideoPlayer::AbrTick() {
  std::deque<double> samples;
  GstElement* demux = nullptr;
  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (burst_bytes_ > 0) {
      if (now - burst_last_rx_ > std::chrono::milliseconds(250)) {
        // Transfer finished and no new segment started yet.
        CloseBurstLocked();
      } else if (now - burst_start_ > std::chrono::seconds(3)) {
        // Saturated link: data flows continuously with no idle gaps, so
        // bursts never close on their own — sample the running transfer.
        CloseBurstLocked();
      }
    }
    samples = abr_samples_;
    if (hls_demux_) demux = GST_ELEMENT(gst_object_ref(hls_demux_));
  }

  if (!demux) return;
  if (samples.size() < 2) {
    gst_object_unref(demux);
    return;
  }

  // --- Prediction ---
  const size_t n = samples.size();
  const size_t window = std::min<size_t>(n, 8);
  double mean = 0.0, hm_denom = 0.0;
  for (size_t i = n - window; i < n; ++i) {
    mean += samples[i];
    hm_denom += 1.0 / samples[i];
  }
  mean /= window;
  const double harmonic = window / hm_denom;  // robust vs. one fast burst
  double variance = 0.0;
  for (size_t i = n - window; i < n; ++i) {
    variance += (samples[i] - mean) * (samples[i] - mean);
  }
  const double cv = mean > 0 ? std::sqrt(variance / window) / mean : 0.0;
  const double fast = (samples[n - 1] + samples[n - 2]) / 2.0;  // recency

  // Sustainable estimate: the cautious of recent vs. long-run, discounted
  // by jitter — an erratic link earns a lower promise than a steady one.
  double predicted_bps = std::min(fast, harmonic);
  predicted_bps *= std::min(1.0, std::max(0.5, 1.0 - 0.5 * cv));

  // --- Buffer health (percent of the 30 s buffering target) ---
  const int pct = last_buffering_percent_.load();
  const double buffer_secs = pct < 0 ? 10.0 :
      (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;

  double safety;
  if (buffer_secs < 6.0) {
    safety = 0.50;  // emergency: survival beats quality
  } else if (buffer_secs < 15.0) {
    safety = 0.65;  // cautious: rebuild cushion first
  } else {
    safety = 0.85;  // comfortable: ride quality close to the estimate
  }

  guint64 target_kbps =
      static_cast<guint64>(predicted_bps * safety / 1000.0);
  if (target_kbps < 100) target_kbps = 100;  // keep the lowest rung reachable

  // --- Anti-flap policy (buffer-aware) ---
  // The buffer, not the raw estimate, decides down-switches. A full cushion
  // proves the current rung is sustainable, so we ignore estimate drops until
  // the buffer actually starts draining — this kills the quality pumping seen
  // when every noisy dip at buffer=30s triggered a "bandwidth drop".
  const auto now = std::chrono::steady_clock::now();
  bool publish = false;
  const char* reason = "";
  const bool is_drop = target_kbps * 5 <= published_kbps_ * 4;  // >=20% below

  if (published_kbps_ == 0) {
    publish = true;
    reason = "first estimate";
    abr_drop_ticks_ = 0;
  } else if (buffer_secs < 6.0 && target_kbps < published_kbps_) {
    // Buffer emergency: cushion nearly gone — any reduction helps NOW. This is
    // the only instant down-switch path. Deliberately does NOT stamp
    // last_downswitch_time_: survival beats the anti-thrash dwell.
    publish = true;
    reason = "buffer emergency";
    abr_drop_ticks_ = 0;
  } else if (is_drop && buffer_secs < kHealthyBufferSecs) {
    // The buffer has meaningfully drained AND the estimate is >=20% down —
    // a real decline. Still require it to persist a few ticks so a single
    // dipped sample can't flap the rung.
    if (++abr_drop_ticks_ >= kSustainedDropTicks) {
      publish = true;
      reason = "bandwidth drop";
      abr_drop_ticks_ = 0;
      last_downswitch_time_ = now;
    } else {
      std::cout << "ABR: drop deferred (" << abr_drop_ticks_ << "/"
                << kSustainedDropTicks << ") buffer="
                << static_cast<int>(buffer_secs) << "s cv="
                << static_cast<int>(cv * 100) << "%" << std::endl;
    }
  } else {
    // Either no drop, or a drop while the buffer is still healthy (absorb it).
    abr_drop_ticks_ = 0;
    // Anti-thrash gate: after a non-emergency down-switch, hold the new rung
    // for kPostDownDwellSecs regardless of estimate before letting it climb
    // again. On device (11:20:29 drop -> 11:20:30 up-switch) the two decisions
    // fired one second apart on the same noisy sample window; the up-switch
    // often resolved to the very rung the drop had just left, producing
    // pipeline-visible churn for zero quality gain.
    const bool post_down_dwell_served =
        last_downswitch_time_.time_since_epoch().count() == 0 ||
        now - last_downswitch_time_ > std::chrono::seconds(kPostDownDwellSecs);
    if (target_kbps * 4 >= published_kbps_ * 5 &&
        buffer_secs >= kHealthyBufferSecs &&
        now - last_upswitch_time_ > std::chrono::seconds(30) &&
        post_down_dwell_served) {
      publish = true;  // >=25% headroom, healthy buffer, both dwell timers served
      reason = "up-switch";
      last_upswitch_time_ = now;
    }
  }

  if (publish) {
    SetConnectionSpeedKbps(demux, target_kbps);
    published_kbps_ = target_kbps;
    std::cout << "ABR: " << reason << " — connection-speed=" << target_kbps
              << "kbps (predicted=" << static_cast<int>(predicted_bps / 1000)
              << "kbps cv=" << static_cast<int>(cv * 100) << "% buffer="
              << static_cast<int>(buffer_secs) << "s safety=" << safety << ")"
              << std::endl;
  } else if (++abr_heartbeat_counter_ % 30 == 0) {
    std::cout << "ABR: holding " << published_kbps_ << "kbps (predicted="
              << static_cast<int>(predicted_bps / 1000) << "kbps cv="
              << static_cast<int>(cv * 100) << "% buffer="
              << static_cast<int>(buffer_secs) << "s samples=" << n << ")"
              << std::endl;
  }

  gst_object_unref(demux);
}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      // A LIVE stream has no end and must never "complete". souphttpsrc can
      // emit a spurious EOS on a live source (live edge, or a closed
      // keep-alive=FALSE connection) where curlhttpsrc did not. Marking
      // is_completed_ then makes GetCurrentPosition either SetSeek(0) (auto-
      // repeat → "Failed to seek" on live) or fire a 'completed' event the app
      // restarts from — BOTH replay the buffered window, and a repeating EOS
      // turns that into the "same buffer loops" bug (which appeared anywhere
      // from 10 to 95 min into playback).
      //
      // Gate on is_live_ (set in Preroll from NO_PREROLL) — the authoritative
      // live signal. The earlier duration>0 check was INERT because a live HLS
      // playlist reports a small sliding-window duration that looks like VOD.
      // On live: ignore EOS entirely; a genuine stop is recovered by the
      // frame-arrival watchdog. On VOD: complete normally.
      if (self->is_live_) {
        std::cout << "EOS on LIVE stream — ignoring (no completion/seek-0; "
                     "frame-arrival watchdog handles a real stop)" << std::endl;
        break;
      }
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

        // Notify buffering update to Flutter (UI only — never drives pipeline
        // state).
        self->stream_handler_->OnNotifyBufferingUpdate(percent);

        // NOTE: We deliberately do NOT pause/resume the pipeline on buffering
        // percentage. For a LIVE stream that approach is unsafe: when the
        // download starves the percent never climbs back to 100, so the resume
        // condition is unreachable and the pipeline wedges in PAUSED forever.
        // Two unsynchronized detached set_state() threads also raced to a
        // nondeterministic final state. Recovery from a real buffer stall is
        // handled by the watchdog Check 1 below (it fires OnNotifyError so
        // Flutter can re-authenticate and rebuild the pipeline).

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
      std::string error_msg = error->message ? error->message : "unknown warning";
      std::cout << "WARNING from " << GST_OBJECT_NAME(message->src)
                << ": " << error_msg;
      if (debug && debug[0]) std::cout << "\n  debug: " << debug;
      std::cout << std::endl;
      if (IsHttpUnavailable(error, debug)) {
        std::cout << "WARNING: treating as entitlement lapse on live stream"
                  << std::endl;
      }
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
      if (IsHttpUnavailable(error, debug)) {
        error_msg = std::string(kStreamUnavailablePrefix) + error_msg;
      }
      g_free(debug);
      g_error_free(error);
      // Stop the watchdog then fire OnNotifyError exactly once, even if the
      // watchdog and GST_MESSAGE_ERROR race at the same 30s boundary.
      self->watchdog_running_.store(false);
      self->watchdog_cv_.notify_all();
      bool expected = false;
      if (self->error_notified_.compare_exchange_strong(expected, true)) {
        self->last_error_ = error_msg;
        self->stream_handler_->OnNotifyError(error_msg);
      }
      break;
    }
    default:
      break;
  }
  return GST_BUS_DROP;
}
