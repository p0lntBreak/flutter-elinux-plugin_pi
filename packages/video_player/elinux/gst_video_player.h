// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_GST_VIDEO_PLAYER_H_
#define PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_GST_VIDEO_PLAYER_H_

#include <gst/gst.h>

#ifdef USE_EGL_IMAGE_DMABUF
#include <gst/allocators/gstdmabuf.h>
#include <gst/gl/egl/egl.h>
#include <gst/gl/gl.h>
#include <gst/video/video.h>
#endif  // USE_EGL_IMAGE_DMABUF

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <map>
#include <thread>

struct AuthHeaders {
  std::map<std::string, std::string> all_headers;  // Store ALL headers as a map
};

#include "video_player_stream_handler.h"

class GstVideoPlayer {
 public:
  GstVideoPlayer(const std::string& uri,
                 std::unique_ptr<VideoPlayerStreamHandler> handler);
  ~GstVideoPlayer();

  static void GstLibraryLoad();
  static void GstLibraryUnload();

  bool Init();
  bool Play();
  bool Pause();
  bool Stop();
  bool SetVolume(double volume);
  bool SetPlaybackRate(double rate);
  // Looping is force-disabled. The app never intends to loop any content (no
  // setLooping originates from soatv); the zeroratehls wrapper enables it by
  // default, which on a LIVE stream turns a spurious souphttpsrc EOS into a
  // SetSeek(0) replay of the buffered window — the "same buffer loops" bug
  // (continuous frames + Current Position resetting ~30s→0). With auto_repeat_
  // pinned false, a live EOS never seeks-to-0; frames just stop and the
  // frame-arrival watchdog reconnects (clean, like curl). Re-enable per-stream
  // via a real live flag if VOD looping is ever genuinely needed.
  void SetAutoRepeat(bool /*auto_repeat*/) { auto_repeat_ = false; };
  bool SetSeek(int64_t position);
  int64_t GetDuration();
  int64_t GetCurrentPosition();
  const uint8_t* GetFrameBuffer();
#ifdef USE_EGL_IMAGE_DMABUF
  void* GetEGLImage(void* egl_display, void* egl_context);
#endif  // USE_EGL_IMAGE_DMABUF
  int32_t GetWidth() const { return width_; };
  int32_t GetHeight() const { return height_; };
  
  // ADD THIS METHOD DECLARATION
  void SetAuthHeaders(const std::map<std::string, std::string>& headers);

  // Last error string set on this player (via NotifyErrorOnce). The plugin's
  // create() reply must carry the specific error text through — the Dart
  // side's event channel is not yet connected when Init() runs, so an
  // OnNotifyError() dispatch at that stage is not observable to Dart, and the
  // create-method-channel reply is the only signal Dart receives on init
  // failure. Returns an empty string if no error was ever recorded.
  std::string GetLastError() const { return last_error_; }

 AuthHeaders auth_headers_;

 private:
  struct GstVideoElements {
    GstElement* pipeline;
    GstElement* playbin;
    GstElement* video_convert;
    GstElement* video_sink;
    GstElement* output;
    GstBus* bus;
    GstBuffer* buffer;
  };

  static void HandoffHandler(GstElement* fakesink, GstBuffer* buf,
                             GstPad* new_pad, gpointer user_data);
  static GstBusSyncReply HandleGstMessage(GstBus* bus, GstMessage* message,
                                          gpointer user_data);
  std::string ParseUri(const std::string& uri);
  bool CreatePipeline();
  void DestroyPipeline();
  bool Preroll();
  void GetVideoSize(int32_t& width, int32_t& height);
  void StartWatchdog();
  void StopWatchdog();
  // Force pipeline recovery when stuck (frames not advancing for several
  // seconds, network healthy, buffer stable). Cycles PAUSED -> PLAYING rather
  // than attempting a seek — the seek-based approach in 74ad4f9 returned
  // false on device 2026-08-03, likely because live HLS pipelines don't
  // accept seek events in their default configuration.
  bool TryFlushRecovery();
  // Append a bus-message summary to bus_msg_ring_. Called from
  // HandleGstMessage on every message. Trims to the last 20 entries.
  void PushBusMsgRing(const std::string& type, const std::string& src_name,
                      const std::string& extra);
  // Print the current bus-message ring buffer to stdout. Called from
  // LogPlaybackHealth when a buffer collapse is detected.
  void DumpBusMsgRing();
  // Fire OnNotifyError at most once (matches the watchdog/error single-fire
  // guard) and stop the watchdog. Used by both the fatal-error path and the
  // entitlement-unavailable path so they never double-notify.
  void NotifyErrorOnce(const std::string& message);
  void StartAbrEngine();
  void StopAbrEngine();
  void AbrTick();
  void CloseBurstLocked();
  void LogPlaybackHealth(GstState state, uint64_t frames,
                         std::chrono::steady_clock::time_point now,
                         std::chrono::steady_clock::time_point last_frame_time);
  static void DeepElementAddedHandler(GstBin* bin, GstBin* sub_bin,
                                      GstElement* element, gpointer user_data);
  static GstPadProbeReturn AbrThroughputProbe(GstPad* pad,
                                              GstPadProbeInfo* info,
                                              gpointer user_data);
#ifdef USE_EGL_IMAGE_DMABUF
  void UnrefEGLImage();
#endif  // USE_EGL_IMAGE_DMABUF

  GstVideoElements gst_;
  // Held reference to the "volume" element inside our custom audio bin. We
  // wrap alsasink in a bin, which means playbin's own "mute" property has no
  // GstStreamVolume interface to forward to on this pipeline (the alsasink
  // bin doesn't implement one), and mute silently no-ops. Driving this
  // element directly gives us a working mute during the preroll gate.
  GstElement* audio_volume_ = nullptr;
  std::string uri_;
  // Cold-start rung hint parsed from a `#soatv:startup_kbps=N` fragment on
  // the URI. When non-zero, overrides kColdStartConnSpeedKbps at pipeline
  // construction so hlsdemux picks a rendition suited to the current
  // network measurement (from soatv's auth-GET throughput probe) or the
  // rung a preceding ABR_RESTART decided on. Zero means "no hint, use
  // the fixed default". Stripped from uri_ before being handed to
  // playbin — playbin doesn't need to see it (GStreamer would ignore
  // it anyway, but keeping the URL clean avoids surprises).
  guint64 startup_kbps_hint_ = 0;
  std::unique_ptr<uint32_t[]> pixels_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  double volume_ = 1.0;
  double playback_rate_ = 1.0;
  bool mute_ = false;
  bool auto_repeat_ = false;
  bool is_completed_ = false;
  // True when the source is LIVE (set in Preroll from GST_STATE_CHANGE_NO_PREROLL).
  // A live stream has no end: EOS on it is spurious and must never be treated as
  // completion (no seek-0, no 'completed' event) or it loops the buffered window.
  bool is_live_ = false;
  std::mutex mutex_event_completed_;
  std::shared_mutex mutex_buffer_;
  std::unique_ptr<VideoPlayerStreamHandler> stream_handler_;
  std::atomic<int> last_buffering_percent_{-1};
  std::chrono::steady_clock::time_point buffering_log_start_time_ =
      std::chrono::steady_clock::now();

  // Stall watchdog: fires OnNotifyError only when the pipeline is PLAYING but
  // no decoded video frame has reached the sink for kFrameStallTimeoutSecs —
  // i.e. the picture is genuinely frozen. The old buffer-percent trigger was
  // demoted to a diagnostic log: a buffering plateau (e.g. stuck at 78% at the
  // live edge) is NOT a freeze if frames are still being rendered, and firing
  // a full reconnect on it produced false positives that churned pipelines and
  // ended in a crash. frames_handed_off_ moves only on a real buffer at the
  // video sink (HandoffHandler), so it is the ground-truth liveness signal.
  std::thread watchdog_thread_;
  std::atomic<bool> watchdog_running_{false};
  std::atomic<bool> error_notified_{false};  // single-fire guard for OnNotifyError
  // Last error message stored alongside the single-fire guard. Read by
  // GetLastError() so the create-method-channel reply can carry the specific
  // error text (e.g. NETWORK_TOO_SLOW:) when Init() fails. Written only under
  // the same compare_exchange_strong that flips error_notified_ true.
  std::string last_error_;
  // Consecutive HTTP 401/403/410 failures from the HTTP source. A live stream
  // whose subscription/entitlement has lapsed 4xx's every fetch; hlsdemux
  // retries and swallows them (emitting EOS, which we ignore on live), so no
  // fatal bus ERROR ever fires and playback silently loops the buffered
  // remnant. Counting these warnings lets us detect that case and signal
  // Flutter to check the subscription instead of reconnecting forever. Reset
  // to 0 whenever a video frame advances (proof the stream is alive).
  std::atomic<int> consecutive_unauthorized_{0};
  std::atomic<bool> play_state_requested_{false}; // tracks if user requested PLAYING
  std::chrono::steady_clock::time_point last_buffering_progress_time_ =
      std::chrono::steady_clock::now();
  std::atomic<uint64_t> frames_handed_off_{0};  // bumped per video buffer
  // Previous tick's buffer_health_secs, used by LogPlaybackHealth to detect a
  // sharp collapse (e.g. buffer dropping from 30s to 2s in one tick, which we
  // have seen but never captured the cause of). Only touched from the watchdog
  // thread. -1.0 means "no previous sample yet."
  double prev_buffer_health_secs_ = -1.0;
  // Ring buffer of recent bus messages, dumped when a buffer collapse fires.
  // Purpose: identify which bus event caused the collapse (manifest refresh,
  // discontinuity, flush-start, stream-start, etc.) rather than guessing.
  // Written from the streaming thread inside HandleGstMessage; read from the
  // watchdog thread inside LogPlaybackHealth. Protected by bus_msg_ring_mutex_.
  // Kept small (last 20) so the streaming thread stays fast and the log dump
  // stays readable.
  struct BusMsgEntry {
    std::chrono::steady_clock::time_point when;
    std::string type;      // e.g. "STATE_CHANGED", "ELEMENT", "SEGMENT_DONE"
    std::string src_name;  // element that emitted it, e.g. "hlsdemux0"
    std::string extra;     // small type-specific detail (may be empty)
  };
  std::deque<BusMsgEntry> bus_msg_ring_;
  std::mutex bus_msg_ring_mutex_;
  // Last time we did a stuck-recovery flush. Guards against firing again too
  // quickly if the first flush didn't take. Set by the watchdog thread.
  std::chrono::steady_clock::time_point last_flush_recovery_time_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;

  // --- ABR engine (Continuous Playback Intelligence) ---
  // Measures per-segment download throughput with a pad probe on hlsdemux's
  // sink pad, predicts sustainable bandwidth (harmonic mean + jitter
  // discount), modulates it by buffer health, and steers hlsdemux's rendition
  // selection through its connection-speed property (kbps). hlsdemux switches
  // renditions at segment boundaries, so playback never interrupts.
  std::thread abr_thread_;
  std::atomic<bool> abr_running_{false};
  std::mutex abr_mutex_;  // guards hls_demux_, burst state, abr_samples_
  std::condition_variable abr_cv_;
  GstElement* hls_demux_ = nullptr;  // ref held; released in DestroyPipeline
  // Burst accumulator — written from the demuxer streaming thread. A gap in
  // buffer arrivals marks the boundary between segment downloads.
  guint64 burst_bytes_ = 0;
  std::chrono::steady_clock::time_point burst_start_;
  std::chrono::steady_clock::time_point burst_last_rx_;
  // Total bytes seen at the http source pad since Init() began. Read atomically
  // by the preroll gate to distinguish "network is genuinely dead" (bytes not
  // accumulating) from "downstream is throttling" (bytes accumulating but the
  // buffering signal capped). Written from the throughput-probe pad callback
  // (which already holds abr_mutex_), so kept as an atomic for lock-free reads
  // from the preroll thread. Never resets during a pipeline's lifetime.
  std::atomic<uint64_t> total_bytes_fetched_{0};
  // Completed per-burst throughput samples in bits/sec (newest at back).
  std::deque<double> abr_samples_;
  guint64 last_segment_bytes_ = 0;
  double last_segment_download_secs_ = 0.0;
  double last_segment_throughput_bps_ = 0.0;
  // Policy state — touched only by the ABR thread.
  guint64 published_kbps_ = 0;
  std::chrono::steady_clock::time_point last_upswitch_time_;
  // Timestamp of the last non-emergency down-switch. Used to enforce a
  // cross-direction dwell: an up-switch immediately after a down-switch
  // (seen 11:20:29 -> 11:20:30 on device) is quality thrash and often
  // resolves to the same rung, so the second decision was pointless churn.
  // Emergency down-switches deliberately do NOT stamp this — survival still
  // beats hysteresis when the buffer is nearly gone.
  std::chrono::steady_clock::time_point last_downswitch_time_;
  int abr_heartbeat_counter_ = 0;
  // Consecutive AbrTick cycles a >=20% drop has persisted below the buffer
  // floor; a down-switch fires only once it reaches kSustainedDropTicks so a
  // single noisy sample can't flap the rung. Reset whenever the drop clears.
  int abr_drop_ticks_ = 0;
  // Consecutive AbrTick cycles the predicted throughput has been below the
  // currently-published rate. Separate from abr_drop_ticks_ because it fires
  // regardless of buffer health — a sustained undershoot means the network
  // genuinely can't sustain the current rung, so we should down-switch before
  // the buffer drains rather than after. Reset whenever predicted catches up.
  // Device log 2026-07-31 18:22:11-18:23:41 held 1091 kbps published while
  // predicted was 446/517/257/307 kbps across four ticks (with buffer still
  // healthy) — the ABR should have already been on a lower rung.
  int abr_undershoot_ticks_ = 0;
  // Consecutive AbrTick cycles the predicted throughput has been very much
  // HIGHER than the currently-published rate (>=5x). Mirror of undershoot.
  // Fires when the buffer is stuck low and the normal up-switch gate can't
  // open. Device log 2026-08-03 14:14 saw published held at 177 kbps for 6+
  // minutes while predicted showed 6-18 Mbps — the buffer never reached
  // 15 s because 177 kbps starves the pipeline, so up-switch never fired.
  int abr_trapped_ticks_ = 0;

  // --- ABR calm-mode state (task #46, 2026-08-17) ---
  //
  // Timestamp of when this pipeline first reached PLAYING with frames. Set
  // once from the ABR thread on the first tick where published_kbps_ moves
  // from 0 to non-zero. Used by the fragile-up-switch gate to route the
  // first up-switch through ABR_RESTART if it fires within a young-pipeline
  // window (see kFragilePipelineSecs). Never touches anything else.
  std::chrono::steady_clock::time_point first_publish_time_;
  // Timestamp of the most recent BUFFER-COLLAPSE, set from the watchdog
  // thread's LogPlaybackHealth. Atomic because ABR reads it cross-thread.
  // Time-since-epoch of 0 means "never collapsed since pipeline start".
  // Used to gate up-switches during the recovery-from-collapse window
  // (BBC News 2026-08-17 10:47 crashed on an in-place up-switch 4s after
  // a BUFFER-COLLAPSE while playbin was still re-plugging inputselectors).
  std::atomic<int64_t> last_buffer_collapse_ns_{0};
  // Timestamp of the most recent ABR decision of any kind (first-estimate,
  // up-switch, down-switch, emergency, undershoot, trapped-rate escape).
  // Enforces a global post-decision cooldown so we can't fire two ABR
  // decisions in rapid succession. BBC News 2026-08-17 log showed 30 ABR
  // restarts in 37 minutes, driving a 19x buffer-collapse rate vs the
  // FAITH TV single-variant baseline (which had zero switches).
  std::chrono::steady_clock::time_point last_abr_decision_time_;

  // First-frame synchronisation: Init() advances to PLAYING then waits here
  // until HandoffHandler delivers the first decoded frame so that video
  // dimensions are known before OnNotifyInitialized() is called.
  std::atomic<bool> first_frame_ready_{false};
  std::mutex mutex_first_frame_;
  std::condition_variable first_frame_cv_;

  // Guards against calling OnNotifyInitialized() more than once (Init() and
  // HandoffHandler can race on the deferred-init path).
  std::atomic<bool> initialized_{false};

#ifdef USE_EGL_IMAGE_DMABUF
  GstVideoInfo gst_video_info_;
  GstEGLImage* gst_egl_image_ = NULL;
  GstGLContext* gst_gl_ctx_ = NULL;
  GstGLDisplayEGL* gst_gl_display_egl_ = NULL;
#endif  // USE_EGL_IMAGE_DMABUF
};

#endif  // PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_GST_VIDEO_PLAYER_H_
