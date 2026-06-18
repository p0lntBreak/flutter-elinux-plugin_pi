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
  void SetAutoRepeat(bool auto_repeat) { auto_repeat_ = auto_repeat; };
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
  void StartAbrEngine();
  void StopAbrEngine();
  void AbrTick();
  void CloseBurstLocked();
  static void DeepElementAddedHandler(GstBin* bin, GstBin* sub_bin,
                                      GstElement* element, gpointer user_data);
  static GstPadProbeReturn AbrThroughputProbe(GstPad* pad,
                                              GstPadProbeInfo* info,
                                              gpointer user_data);
#ifdef USE_EGL_IMAGE_DMABUF
  void UnrefEGLImage();
#endif  // USE_EGL_IMAGE_DMABUF

  GstVideoElements gst_;
  std::string uri_;
  std::unique_ptr<uint32_t[]> pixels_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  double volume_ = 1.0;
  double playback_rate_ = 1.0;
  bool mute_ = false;
  bool auto_repeat_ = false;
  bool is_completed_ = false;
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
  std::atomic<bool> play_state_requested_{false}; // tracks if user requested PLAYING
  std::chrono::steady_clock::time_point last_buffering_progress_time_ =
      std::chrono::steady_clock::now();
  std::atomic<uint64_t> frames_handed_off_{0};  // bumped per video buffer
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
  // Completed per-burst throughput samples in bits/sec (newest at back).
  std::deque<double> abr_samples_;
  // Policy state — touched only by the ABR thread.
  guint64 published_kbps_ = 0;
  std::chrono::steady_clock::time_point last_upswitch_time_;
  int abr_heartbeat_counter_ = 0;

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
