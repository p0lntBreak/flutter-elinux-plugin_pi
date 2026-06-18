// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_VIDEO_PLAYER_STREAM_HANDLER_IMPL_H_
#define PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_VIDEO_PLAYER_STREAM_HANDLER_IMPL_H_

#include <functional>

#include "video_player_stream_handler.h"

class VideoPlayerStreamHandlerImpl : public VideoPlayerStreamHandler {
 public:
  using OnNotifyInitialized = std::function<void()>;
  using OnNotifyFrameDecoded = std::function<void()>;
  using OnNotifyCompleted = std::function<void()>;
  using OnNotifyPlaying = std::function<void(bool)>;
  using OnNotifyError = std::function<void(const std::string&)>;
  using OnNotifyBufferingStart = std::function<void()>;
  using OnNotifyBufferingUpdate = std::function<void(int)>;
  using OnNotifyBufferingEnd = std::function<void()>;

  VideoPlayerStreamHandlerImpl(OnNotifyInitialized on_notify_initialized,
                               OnNotifyFrameDecoded on_notify_frame_decoded,
                               OnNotifyCompleted on_notify_completed,
                               OnNotifyPlaying on_notify_playing,
                               OnNotifyError on_notify_error,
                               OnNotifyBufferingStart on_notify_buffering_start,
                               OnNotifyBufferingUpdate on_notify_buffering_update,
                               OnNotifyBufferingEnd on_notify_buffering_end)
      : on_notify_initialized_(on_notify_initialized),
        on_notify_frame_decoded_(on_notify_frame_decoded),
        on_notify_completed_(on_notify_completed),
        on_notify_playing_(on_notify_playing),
        on_notify_error_(on_notify_error),
        on_notify_buffering_start_(on_notify_buffering_start),
        on_notify_buffering_update_(on_notify_buffering_update),
        on_notify_buffering_end_(on_notify_buffering_end) {}
  virtual ~VideoPlayerStreamHandlerImpl() = default;

  // Prevent copying.
  VideoPlayerStreamHandlerImpl(VideoPlayerStreamHandlerImpl const&) = delete;
  VideoPlayerStreamHandlerImpl& operator=(VideoPlayerStreamHandlerImpl const&) =
      delete;

 protected:
  // |VideoPlayerStreamHandler|
  void OnNotifyInitializedInternal() override {
    if (on_notify_initialized_) {
      on_notify_initialized_();
    }
  }

  // |VideoPlayerStreamHandler|
  void OnNotifyFrameDecodedInternal() override {
    if (on_notify_frame_decoded_) {
      on_notify_frame_decoded_();
    }
  }

  // |VideoPlayerStreamHandler|
  void OnNotifyCompletedInternal() override {
    if (on_notify_completed_) {
      on_notify_completed_();
    }
  }

  void OnNotifyPlayingInternal(bool is_playing) override {
    if (on_notify_playing_) {
      on_notify_playing_(is_playing);
    }
  }

  void OnNotifyErrorInternal(const std::string& message) override {
    if (on_notify_error_) {
      on_notify_error_(message);
    }
  }

  void OnNotifyBufferingStartInternal() override {
    if (on_notify_buffering_start_) {
      on_notify_buffering_start_();
    }
  }

  void OnNotifyBufferingUpdateInternal(int percent) override {
    if (on_notify_buffering_update_) {
      on_notify_buffering_update_(percent);
    }
  }

  void OnNotifyBufferingEndInternal() override {
    if (on_notify_buffering_end_) {
      on_notify_buffering_end_();
    }
  }

  OnNotifyInitialized on_notify_initialized_;
  OnNotifyFrameDecoded on_notify_frame_decoded_;
  OnNotifyCompleted on_notify_completed_;
  OnNotifyPlaying on_notify_playing_;
  OnNotifyError on_notify_error_;
  OnNotifyBufferingStart on_notify_buffering_start_;
  OnNotifyBufferingUpdate on_notify_buffering_update_;
  OnNotifyBufferingEnd on_notify_buffering_end_;
};

#endif  // PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_VIDEO_PLAYER_STREAM_HANDLER_IMPL_H_
