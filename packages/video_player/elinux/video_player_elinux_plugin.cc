// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/video_player_elinux/video_player_elinux_plugin.h"

#include <flutter/basic_message_channel.h>
#include <flutter/encodable_value.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_message_codec.h>
#include <flutter/standard_method_codec.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gst_video_player.h"
#include "messages/messages.h"
#include "video_player_stream_handler_impl.h"

namespace {
constexpr char kVideoPlayerApiChannelInitializeName[] =
    "dev.flutter.pigeon.VideoPlayerApi.initialize";
constexpr char kVideoPlayerApiChannelSetMixWithOthersName[] =
    "dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers";
constexpr char kVideoPlayerApiChannelCreateName[] =
    "dev.flutter.pigeon.VideoPlayerApi.create";
constexpr char kVideoPlayerApiChannelDisposeName[] =
    "dev.flutter.pigeon.VideoPlayerApi.dispose";
constexpr char kVideoPlayerApiChannelSetLoopingName[] =
    "dev.flutter.pigeon.VideoPlayerApi.setLooping";
constexpr char kVideoPlayerApiChannelSetVolumeName[] =
    "dev.flutter.pigeon.VideoPlayerApi.setVolume";
constexpr char kVideoPlayerApiChannelPauseName[] =
    "dev.flutter.pigeon.VideoPlayerApi.pause";
constexpr char kVideoPlayerApiChannelPlayName[] =
    "dev.flutter.pigeon.VideoPlayerApi.play";
constexpr char kVideoPlayerApiChannelPositionName[] =
    "dev.flutter.pigeon.VideoPlayerApi.position";
constexpr char kVideoPlayerApiChannelSetPlaybackSpeedName[] =
    "dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed";
constexpr char kVideoPlayerApiChannelSeekToName[] =
    "dev.flutter.pigeon.VideoPlayerApi.seekTo";

constexpr char kVideoPlayerVideoEventsChannelName[] =
    "flutter.io/videoPlayer/videoEvents";

constexpr char kEncodableMapkeyResult[] = "result";
constexpr char kEncodableMapkeyError[] = "error";

void RecordDiagnosticMaximum(std::atomic<uint64_t>& maximum,
                             uint64_t value) {
  uint64_t observed = maximum.load(std::memory_order_relaxed);
  while (observed < value &&
         !maximum.compare_exchange_weak(observed, value,
                                        std::memory_order_relaxed)) {
  }
}

class VideoPlayerPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  VideoPlayerPlugin(flutter::PluginRegistrar* plugin_registrar,
                    flutter::TextureRegistrar* texture_registrar)
      : plugin_registrar_(plugin_registrar),
        texture_registrar_(texture_registrar) {
    // Needs to call 'gst_init' that initializing the GStreamer library before
    // using it.
    GstVideoPlayer::GstLibraryLoad();
  }
  virtual ~VideoPlayerPlugin() {
    // DisposePlayer() erases from players_ itself and parks the shell in
    // retired_players_, so snapshot the ids first and don't erase here.
    std::vector<int64_t> texture_ids;
    texture_ids = PlayerIds();
    for (const auto texture_id : texture_ids) {
      DisposePlayer(texture_id);
    }
    // Every GstVideoPlayer was destroyed synchronously inside DisposePlayer;
    // the retired shells hold no GStreamer objects. Unregister the textures
    // now (deferred at dispose per task #38) before dropping the shells and
    // unloading the library — at shutdown there is no raster thread still
    // rendering to race against, so it is safe to unregister immediately.
    std::vector<std::shared_ptr<FlutterVideoPlayer>> retired_players;
    {
      std::lock_guard<std::mutex> guard(players_mutex_);
      for (auto& retired : retired_players_) {
        retired_players.push_back(retired.second);
      }
      retired_players_.clear();
    }
    for (const auto& retired : retired_players) {
      texture_registrar_->UnregisterTexture(retired->texture_id);
    }
    GstVideoPlayer::GstLibraryUnload();
  }

 private:
  struct FlutterVideoPlayer {
    struct TextureDiagnosticStats {
      std::atomic<uint64_t> decoded_frames{0};
      std::atomic<uint64_t> texture_marked{0};
      std::atomic<uint64_t> texture_coalesced{0};
      std::atomic<uint64_t> texture_mark_failed{0};
      std::atomic<uint64_t> texture_callbacks{0};
      std::atomic<uint64_t> texture_returned{0};
      std::atomic<uint64_t> texture_lock_busy{0};
      std::atomic<uint64_t> texture_no_player{0};
      std::atomic<uint64_t> texture_no_frame{0};
      std::atomic<uint64_t> buffer_releases{0};
      std::atomic<uint64_t> copy_time_ns{0};
      std::atomic<uint64_t> copy_time_samples{0};
      std::atomic<uint64_t> copy_time_max_ns{0};
      std::atomic<uint64_t> mark_to_callback_time_ns{0};
      std::atomic<uint64_t> mark_to_callback_samples{0};
      std::atomic<uint64_t> mark_to_callback_max_ns{0};
      std::atomic<uint64_t> buffer_hold_time_ns{0};
      std::atomic<uint64_t> buffer_hold_time_samples{0};
      std::atomic<uint64_t> buffer_hold_time_max_ns{0};
    } texture_diagnostics;

    int64_t texture_id;
    // Shared ownership lets callbacks keep the native player alive while a
    // disposal/reconnect removes it from the plugin registry. The registry
    // owns the normal lifetime; an in-flight texture callback may hold the
    // last reference until Flutter releases the returned pixel buffer.
    std::shared_ptr<GstVideoPlayer> player;
    std::unique_ptr<flutter::TextureVariant> texture;
    std::unique_ptr<FlutterDesktopPixelBuffer> buffer;
    // Serialises the pixel-buffer copy (and the engine's subsequent
    // glTexImage2D read, released via the buffer's release_callback) against
    // DisposePlayer() tearing down `player`. See DisposePlayer().
    std::mutex buffer_mutex;
    std::shared_ptr<GstVideoPlayer> in_flight_player;
    std::mutex player_mutex;

    std::shared_ptr<GstVideoPlayer> GetPlayer() {
      std::lock_guard<std::mutex> guard(player_mutex);
      return player;
    }

    void ClearPlayer() {
      std::lock_guard<std::mutex> guard(player_mutex);
      player.reset();
    }
    // Flutter posts both a raster task and a UI frame for every
    // MarkTextureFrameAvailable call. Keep at most one notification in flight
    // so a sustained live stream cannot build an input-blocking task backlog.
    std::atomic<bool> texture_frame_pending{false};
    std::atomic<int64_t> texture_mark_started_at_ns{-1};
    bool texture_diagnostics_enabled = false;
    std::atomic<bool> texture_diagnostics_stopping{false};
    std::mutex texture_diagnostics_wait_mutex;
    std::condition_variable texture_diagnostics_wait_cv;
    std::thread texture_diagnostics_thread;
    std::chrono::steady_clock::time_point texture_buffer_returned_at;
#ifdef USE_EGL_IMAGE_DMABUF
    std::unique_ptr<FlutterDesktopEGLImage> egl_image;
#endif  // USE_EGL_IMAGE_DMABUF
    std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
        event_channel;
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink;
    std::mutex error_delivery_mutex;
    std::string pending_error;
    std::atomic<bool> error_reported{false};

    void DeliverError(const std::string& message) {
      std::lock_guard<std::mutex> guard(error_delivery_mutex);
      error_reported.store(true);
      if (!event_sink) {
        pending_error = message;
        return;
      }
      event_sink->Error("VideoError", message);
    }
    // Native startup runs asynchronously so Dart can subscribe to the event
    // channel immediately after create() returns.
    std::thread initialization_thread;
    std::atomic<bool> initialized_event_sent{false};
  };

  void HandleInitializeMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleCreateMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleDisposeMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandlePauseMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandlePlayMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleSetLoopingMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleSetVolumeMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleSetMixWithOthersMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleSetPlaybackSpeedMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandleSeekToMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);
  void HandlePositionMethodCall(
      const flutter::EncodableValue& message,
      flutter::MessageReply<flutter::EncodableValue> reply);

  void SendInitializedEventMessage(int64_t texture_id);
  void SendPlayCompletedEventMessage(int64_t texture_id);
  void SendIsPlayingStateUpdate(int64_t texture_id, bool is_playing);
  void SendErrorEventMessage(int64_t texture_id, const std::string& message);

  void DisposePlayer(int64_t texture_id);
  void StartTextureDiagnostics(
      const std::shared_ptr<FlutterVideoPlayer>& player);
  void StopTextureDiagnostics(
      const std::shared_ptr<FlutterVideoPlayer>& player);
  void ReportTextureDiagnostics(
      const std::shared_ptr<FlutterVideoPlayer>& player);
  void ReapRetiredPlayers();
  std::shared_ptr<FlutterVideoPlayer> FindPlayer(int64_t texture_id);
  std::vector<int64_t> PlayerIds();

  flutter::EncodableValue WrapError(const std::string& message,
                                    const std::string& code = std::string(),
                                    const std::string& details = std::string());

  const std::string GetExecutableDirectory();

  flutter::PluginRegistrar* plugin_registrar_;
  flutter::TextureRegistrar* texture_registrar_;
  std::mutex players_mutex_;
  std::unordered_map<int64_t, std::shared_ptr<FlutterVideoPlayer>> players_;
  // Disposed players whose FlutterVideoPlayer shell is kept alive briefly after
  // UnregisterTexture(): the deprecated synchronous UnregisterTexture(int64_t)
  // does not wait for the raster thread to stop invoking the texture callback,
  // so the object (and its buffer_mutex) must outlive dispose. Reclaimed by
  // ReapRetiredPlayers() once the raster thread has certainly drained.
  std::vector<std::pair<std::chrono::steady_clock::time_point,
                        std::shared_ptr<FlutterVideoPlayer>>>
      retired_players_;
};

// static
void VideoPlayerPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<VideoPlayerPlugin>(
      registrar, registrar->texture_registrar());

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelInitializeName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleInitializeMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelCreateName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleCreateMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelDisposeName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleDisposeMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelPauseName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandlePauseMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelPlayName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandlePlayMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelSetLoopingName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleSetLoopingMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelSetVolumeName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleSetVolumeMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelSetMixWithOthersName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleSetMixWithOthersMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelSetPlaybackSpeedName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleSetPlaybackSpeedMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelSeekToName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandleSeekToMethodCall(message, reply);
        });
  }

  {
    auto channel =
        std::make_unique<flutter::BasicMessageChannel<flutter::EncodableValue>>(
            registrar->messenger(), kVideoPlayerApiChannelPositionName,
            &flutter::StandardMessageCodec::GetInstance());
    channel->SetMessageHandler(
        [plugin_pointer = plugin.get()](const auto& message, auto reply) {
          plugin_pointer->HandlePositionMethodCall(message, reply);
        });
  }

  registrar->AddPlugin(std::move(plugin));
}

void VideoPlayerPlugin::HandleInitializeMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  // Dispose of all existing players. This helps to shut down existing players
  // on a hot restart.
  // https://github.com/flutter/flutter/issues/10437
  // DisposePlayer() erases from players_ itself, so snapshot the ids first.
  std::vector<int64_t> texture_ids = PlayerIds();
  for (const auto texture_id : texture_ids) {
    DisposePlayer(texture_id);
  }

  flutter::EncodableMap result;

  result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                 flutter::EncodableValue());
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleCreateMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  // Sweep any retired players whose 5 s deferred-UnregisterTexture window
  // has elapsed. On a normal reconnect flow this fires shortly after
  // DisposePlayer (Create is typically called ~3 s after Dispose), so
  // retired entries get cleaned up promptly on natural user activity.
  ReapRetiredPlayers();

  auto meta = CreateMessage::FromMap(message);
  std::string uri;
  if (!meta.GetAsset().empty()) {
    // todo: gets propery path of the Flutter project.
    std::string flutter_project_path = GetExecutableDirectory() + "/data/";
    uri = flutter_project_path + "flutter_assets/" + meta.GetAsset();
  } else {
    uri = meta.GetUri();
  }

  auto instance = std::make_unique<FlutterVideoPlayer>();

#ifdef USE_EGL_IMAGE_DMABUF
  instance->egl_image = std::make_unique<FlutterDesktopEGLImage>();
  instance->texture =
      std::make_unique<flutter::TextureVariant>(flutter::EGLImageTexture(
          [instance = instance.get()](
              size_t width, size_t height, void* egl_display,
              void* egl_context) -> const FlutterDesktopEGLImage* {
            const int64_t mark_started_at_ns =
                instance->texture_diagnostics_enabled
                    ? instance->texture_mark_started_at_ns.exchange(
                          -1, std::memory_order_relaxed)
                    : -1;
            instance->texture_frame_pending.store(
                false, std::memory_order_release);
            if (instance->texture_diagnostics_enabled) {
              instance->texture_diagnostics.texture_callbacks.fetch_add(
                  1, std::memory_order_relaxed);
              if (mark_started_at_ns >= 0) {
                const uint64_t latency_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count() -
                    mark_started_at_ns);
                instance->texture_diagnostics.mark_to_callback_time_ns
                    .fetch_add(latency_ns, std::memory_order_relaxed);
                instance->texture_diagnostics.mark_to_callback_samples
                    .fetch_add(1, std::memory_order_relaxed);
                RecordDiagnosticMaximum(
                    instance->texture_diagnostics
                        .mark_to_callback_max_ns,
                    latency_ns);
              }
            }
            std::shared_ptr<GstVideoPlayer> player = instance->GetPlayer();
            if (!player) {
              if (instance->texture_diagnostics_enabled) {
                instance->texture_diagnostics.texture_no_player.fetch_add(
                    1, std::memory_order_relaxed);
              }
              return nullptr;
            }
            instance->egl_image->width = player->GetWidth();
            instance->egl_image->height = player->GetHeight();
            instance->egl_image->egl_image =
                player->GetEGLImage(egl_display, egl_context);
            if (instance->texture_diagnostics_enabled) {
              if (instance->egl_image->egl_image) {
                instance->texture_diagnostics.texture_returned.fetch_add(
                    1, std::memory_order_relaxed);
              } else {
                instance->texture_diagnostics.texture_no_frame.fetch_add(
                    1, std::memory_order_relaxed);
              }
            }
            return instance->egl_image.get();
          }));
#else
  instance->buffer = std::make_unique<FlutterDesktopPixelBuffer>();
  instance->texture =
      std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
          [instance = instance.get()](
              size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
            const int64_t mark_started_at_ns =
                instance->texture_diagnostics_enabled
                    ? instance->texture_mark_started_at_ns.exchange(
                          -1, std::memory_order_relaxed)
                    : -1;
            instance->texture_frame_pending.store(
                false, std::memory_order_release);
            if (instance->texture_diagnostics_enabled) {
              instance->texture_diagnostics.texture_callbacks.fetch_add(
                  1, std::memory_order_relaxed);
              if (mark_started_at_ns >= 0) {
                const uint64_t latency_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count() -
                    mark_started_at_ns);
                instance->texture_diagnostics.mark_to_callback_time_ns
                    .fetch_add(latency_ns, std::memory_order_relaxed);
                instance->texture_diagnostics.mark_to_callback_samples
                    .fetch_add(1, std::memory_order_relaxed);
                RecordDiagnosticMaximum(
                    instance->texture_diagnostics
                        .mark_to_callback_max_ns,
                    latency_ns);
              }
            }
            // Hold buffer_mutex across the whole copy AND the engine's later
            // glTexImage2D read: the engine reads the returned buffer after
            // this callback returns and signals completion via the buffer's
            // release_callback, so the lock is released there (or on an early
            // return here). DisposePlayer() takes the same lock before freeing
            // `player`, so the frame can't be freed mid-read; and the shell is
            // kept alive past dispose (retired_players_), so this never
            // dereferences freed memory even if it fires after dispose.
            // Never wait on the raster thread. If the engine still owns the
            // previous pixel buffer, retain that frame and try again when the
            // next decoded frame arrives. Blocking here delays every Flutter
            // overlay, including remote-driven media controls.
            if (!instance->buffer_mutex.try_lock()) {
              if (instance->texture_diagnostics_enabled) {
                instance->texture_diagnostics.texture_lock_busy.fetch_add(
                    1, std::memory_order_relaxed);
              }
              return nullptr;
            }
            std::shared_ptr<GstVideoPlayer> player = instance->GetPlayer();
            if (!player) {
              if (instance->texture_diagnostics_enabled) {
                instance->texture_diagnostics.texture_no_player.fetch_add(
                    1, std::memory_order_relaxed);
              }
              instance->buffer_mutex.unlock();
              return nullptr;
            }
            // Keep the native player alive until Flutter invokes the release
            // callback. The engine may read the returned pixel pointer after
            // this callback returns.
            instance->in_flight_player = player;
            std::chrono::steady_clock::time_point copy_started_at;
            if (instance->texture_diagnostics_enabled) {
              copy_started_at = std::chrono::steady_clock::now();
            }
            const uint8_t* frame = player->GetFrameBuffer();
            if (instance->texture_diagnostics_enabled) {
              const uint64_t copy_time_ns = static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - copy_started_at)
                      .count());
              instance->texture_diagnostics.copy_time_ns.fetch_add(
                  copy_time_ns, std::memory_order_relaxed);
              instance->texture_diagnostics.copy_time_samples.fetch_add(
                  1, std::memory_order_relaxed);
              RecordDiagnosticMaximum(
                  instance->texture_diagnostics.copy_time_max_ns,
                  copy_time_ns);
            }
            if (!frame) {
              if (instance->texture_diagnostics_enabled) {
                instance->texture_diagnostics.texture_no_frame.fetch_add(
                    1, std::memory_order_relaxed);
              }
              instance->in_flight_player.reset();
              instance->buffer_mutex.unlock();
              return nullptr;
            }
            instance->buffer->width = player->GetWidth();
            instance->buffer->height = player->GetHeight();
            instance->buffer->buffer = frame;
            instance->buffer->release_callback = [](void* ctx) {
              auto* player = static_cast<FlutterVideoPlayer*>(ctx);
              if (player->texture_diagnostics_enabled) {
                const uint64_t hold_time_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        player->texture_buffer_returned_at)
                        .count());
                player->texture_diagnostics.buffer_releases.fetch_add(
                    1, std::memory_order_relaxed);
                player->texture_diagnostics.buffer_hold_time_ns.fetch_add(
                    hold_time_ns, std::memory_order_relaxed);
                player->texture_diagnostics.buffer_hold_time_samples.fetch_add(
                    1, std::memory_order_relaxed);
                RecordDiagnosticMaximum(
                    player->texture_diagnostics.buffer_hold_time_max_ns,
                    hold_time_ns);
              }
              // Drop the final native-player reference only after releasing
              // buffer_mutex. GstVideoPlayer destruction can stop GStreamer
              // callbacks and must never run while the texture lock is held.
              std::shared_ptr<GstVideoPlayer> in_flight;
              in_flight.swap(player->in_flight_player);
              player->buffer_mutex.unlock();
            };
            instance->buffer->release_context = instance;
            if (instance->texture_diagnostics_enabled) {
              instance->texture_buffer_returned_at =
                  std::chrono::steady_clock::now();
              instance->texture_diagnostics.texture_returned.fetch_add(
                  1, std::memory_order_relaxed);
            }
            return instance->buffer.get();
          }));
#endif  // USE_EGL_IMAGE_DMABUF
  const auto texture_id =
      texture_registrar_->RegisterTexture(instance->texture.get());
  instance->texture_id = texture_id;
  {
    auto event_channel =
        std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
            plugin_registrar_->messenger(),
            kVideoPlayerVideoEventsChannelName + std::to_string(texture_id),
            &flutter::StandardMethodCodec::GetInstance());
    auto event_channel_handler = std::make_unique<
        flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
        [instance = instance.get(), host = this](
            const flutter::EncodableValue* arguments,
            std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                events)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          bool replayed_error = false;
          {
            std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
            instance->event_sink = std::move(events);
            if (!instance->pending_error.empty()) {
              instance->event_sink->Error("VideoError", instance->pending_error);
              instance->pending_error.clear();
              replayed_error = true;
            }
          }
          if (auto player = instance->GetPlayer()) {
            player->LogPlaybackStartup(
                "plugin_event_channel_listening");
          }
          // If native initialization already completed before Dart attached,
          // replay the event now. Otherwise Init() will send it when the first
          // frame and startup buffer gate are ready.
          auto player = instance->GetPlayer();
          if (!replayed_error && player && player->IsInitialized()) {
            host->SendInitializedEventMessage(instance->texture_id);
          }
          return nullptr;
        },
        [instance = instance.get()](const flutter::EncodableValue* arguments)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
          instance->event_sink = nullptr;
          return nullptr;
        });
    event_channel->SetStreamHandler(std::move(event_channel_handler));
    instance->event_channel = std::move(event_channel);
  }
  {
    auto player_handler = std::make_unique<VideoPlayerStreamHandlerImpl>(
        // OnNotifyInitialized
        [texture_id, host = this]() {
          host->SendInitializedEventMessage(texture_id);
        },
        // OnNotifyFrameDecoded
        [texture_id, instance = instance.get(), host = this]() {
          if (instance->texture_diagnostics_enabled) {
            instance->texture_diagnostics.decoded_frames.fetch_add(
                1, std::memory_order_relaxed);
          }
          bool expected = false;
          if (!instance->texture_frame_pending.compare_exchange_strong(
                  expected, true, std::memory_order_acq_rel)) {
            if (instance->texture_diagnostics_enabled) {
              instance->texture_diagnostics.texture_coalesced.fetch_add(
                  1, std::memory_order_relaxed);
            }
            return;
          }
          if (instance->texture_diagnostics_enabled) {
            instance->texture_mark_started_at_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
          }
          if (!host->texture_registrar_->MarkTextureFrameAvailable(
                  texture_id)) {
            if (instance->texture_diagnostics_enabled) {
              instance->texture_diagnostics.texture_mark_failed.fetch_add(
                  1, std::memory_order_relaxed);
              instance->texture_mark_started_at_ns.store(
                  -1, std::memory_order_relaxed);
            }
            instance->texture_frame_pending.store(
                false, std::memory_order_release);
          } else if (instance->texture_diagnostics_enabled) {
            instance->texture_diagnostics.texture_marked.fetch_add(
                1, std::memory_order_relaxed);
          }
        },
        // OnNotifyCompleted
        [texture_id, host = this]() {
          host->SendPlayCompletedEventMessage(texture_id);
        },
        // OnNotifyPlaying
        [texture_id, host = this](bool is_playing) {
          host->SendIsPlayingStateUpdate(texture_id, is_playing);
        },
        // OnNotifyError
        [texture_id, host = this](const std::string& message) {
          host->SendErrorEventMessage(texture_id, message);
        },
        // OnNotifyBufferingStart
        [texture_id, host = this]() {
          auto instance = host->FindPlayer(texture_id);
          if (instance) {
            std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
            if (!instance->event_sink) return;
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingStart")}};
            flutter::EncodableValue event(encodables);
            instance->event_sink->Success(event);
          }
        },
        // OnNotifyBufferingUpdate
        [texture_id, host = this](int percent) {
          auto instance = host->FindPlayer(texture_id);
          if (instance) {
            std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
            if (!instance->event_sink) return;
            auto player = instance->GetPlayer();
            if (!player) return;
            auto duration = player->GetDuration();
            if (duration < 0) duration = 0;
            int64_t buffered_end = (duration * percent) / 100;

            flutter::EncodableList ranges = {
                flutter::EncodableValue(flutter::EncodableList{
                    flutter::EncodableValue(static_cast<int64_t>(0)),
                    flutter::EncodableValue(buffered_end)})};
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingUpdate")},
                {flutter::EncodableValue("values"),
                 flutter::EncodableValue(ranges)}};
            flutter::EncodableValue event(encodables);
            instance->event_sink->Success(event);
          }
        },
        // OnNotifyBufferingEnd
        [texture_id, host = this]() {
          auto instance = host->FindPlayer(texture_id);
          if (instance) {
            std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
            if (!instance->event_sink) return;
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingEnd")}};
            flutter::EncodableValue event(encodables);
            instance->event_sink->Success(event);
          }
        });

    instance->player =
        std::make_shared<GstVideoPlayer>(
            uri, std::move(player_handler), meta.GetSupportedVideoCodecs());

    // Extract and apply HTTP headers dynamically
    const auto& http_headers = meta.GetHttpHeaders();
    if (!http_headers.empty()) {
      std::cout << "Received " << http_headers.size()
                << " HTTP headers from Flutter" << std::endl;

      // Log all headers
      for (const auto& [key, value] : http_headers) {
        std::cout << "  Header: " << key << " = " << value << std::endl;
      }

      // Pass ALL headers to the player
      std::cout << "Setting ALL HTTP headers on player" << std::endl;
      if (auto player = instance->GetPlayer()) {
        player->SetAuthHeaders(http_headers);
      }
    } else {
      std::cout << "No HTTP headers provided from Flutter" << std::endl;
    }

    {
      std::lock_guard<std::mutex> guard(players_mutex_);
      players_[texture_id] = std::move(instance);
    }
  }

  flutter::EncodableMap value;
  TextureMessage result;

  auto player_entry = FindPlayer(texture_id);
  if (!player_entry || !player_entry->GetPlayer()) {
    result.SetTextureId(texture_id);
    value.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                  result.ToMap());
    reply(flutter::EncodableValue(value));
    return;
  }
  auto player = player_entry->GetPlayer();
  if (!player) return;
  StartTextureDiagnostics(player_entry);
  player->LogPlaybackStartup("plugin_create_reply_ready");
  result.SetTextureId(texture_id);
  value.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                result.ToMap());

  // Do not block the platform message handler on GStreamer preroll. The Dart
  // video controller subscribes to the EventChannel only after this reply;
  // keeping Init() synchronous created the multi-second event-channel gap.
  player->LogPlaybackStartup("plugin_init_call_started");
  player_entry->initialization_thread = std::thread(
      [player_entry, texture_id]() {
        auto player = player_entry->GetPlayer();
        if (!player) return;
        player->LogPlaybackStartup("plugin_async_init_started");
        const bool ok = player->Init();
        player->LogPlaybackStartup(
            "plugin_init_returned",
            std::string("success=") + (ok ? "true" : "false"));
        if (!ok) {
          const std::string specific = player->GetLastError();
          const std::string error_message =
              !specific.empty()
                  ? specific
                  : ("Failed to initialize the player with texture id: " +
                     std::to_string(texture_id));
          // The shared entry stays alive even if disposal removed its map key.
          player_entry->DeliverError(error_message);
        }
      });
  reply(flutter::EncodableValue(value));
}

void VideoPlayerPlugin::HandleDisposeMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = TextureMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (FindPlayer(texture_id)) {
    // DisposePlayer() erases from players_ itself.
    DisposePlayer(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandlePauseMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = TextureMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    // Diagnostic: a pipeline stuck in PAUSED traces back to whoever sent this.
    std::cout << "PAUSE method call from Dart for texture " << texture_id
              << std::endl;
    if (auto player = instance->GetPlayer()) player->Pause();
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandlePlayMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = TextureMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    if (auto player = instance->GetPlayer()) player->Play();
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleSetLoopingMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = LoopingMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    if (auto player = instance->GetPlayer())
      player->SetAutoRepeat(parameter.GetIsLooping());
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleSetVolumeMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = VolumeMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    if (auto player = instance->GetPlayer())
      player->SetVolume(parameter.GetVolume());
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleSetMixWithOthersMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  // todo: implements here.

  flutter::EncodableMap result;
  result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                 flutter::EncodableValue());
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandlePositionMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = TextureMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    auto player = instance->GetPlayer();
    if (!player) {
      result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                     flutter::EncodableValue(WrapError(
                         "Player is being disposed", "player_disposed")));
      reply(flutter::EncodableValue(result));
      return;
    }
    auto position = player->GetCurrentPosition();
    // Treat negative positions as 0 instead of returning an error.
    // This can happen during initial buffering or transient GStreamer states.
    if (position < 0) {
      std::cerr << "HandlePositionMethodCall: Got negative position ("
                << position << ") for texture id: " << texture_id
                << " - clamping to 0" << std::endl;
      position = 0;
    }

    PositionMessage send_message;
    send_message.SetTextureId(texture_id);
    send_message.SetPosition(position);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   send_message.ToMap());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleSetPlaybackSpeedMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = PlaybackSpeedMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    if (auto player = instance->GetPlayer())
      player->SetPlaybackRate(parameter.GetSpeed());
    result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                   flutter::EncodableValue());
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::HandleSeekToMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = PositionMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (auto instance = FindPlayer(texture_id)) {
    auto player = instance->GetPlayer();
    if (player && player->SetSeek(parameter.GetPosition())) {
      result.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                     flutter::EncodableValue());
    } else {
      result.emplace(
          flutter::EncodableValue(kEncodableMapkeyError),
          flutter::EncodableValue(WrapError(
              "Native pipeline rejected the seek request", "seek_failed")));
    }
  } else {
    auto error_message = "Couldn't find the player with texture id: " +
                         std::to_string(texture_id);
    result.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                   flutter::EncodableValue(WrapError(error_message)));
  }
  reply(flutter::EncodableValue(result));
}

void VideoPlayerPlugin::SendInitializedEventMessage(int64_t texture_id) {
  const auto instance = FindPlayer(texture_id);
  if (!instance) return;
  std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
  if (instance->error_reported.load()) return;
  auto player = instance->GetPlayer();
  if (!player) return;

  player->LogPlaybackStartup(
      "plugin_initialized_event_attempt",
      std::string("sinkReady=") +
          (instance->event_sink ? "true" : "false"));
  if (!instance->event_sink) return;
  if (!player->IsInitialized()) return;
  bool expected = false;
  if (!instance->initialized_event_sent.compare_exchange_strong(
          expected, true)) {
    return;
  }

  auto duration = player->GetDuration();
  auto width = player->GetWidth();
  auto height = player->GetHeight();
  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"),
       flutter::EncodableValue("initialized")},
      {flutter::EncodableValue("duration"), flutter::EncodableValue(duration)},
      {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
      {flutter::EncodableValue("height"), flutter::EncodableValue(height)}};
  flutter::EncodableValue event(encodables);
  instance->event_sink->Success(event);
  player->LogPlaybackStartup("plugin_initialized_event_sent");
}

void VideoPlayerPlugin::SendPlayCompletedEventMessage(int64_t texture_id) {
  const auto instance = FindPlayer(texture_id);
  if (!instance) return;
  std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
  if (!instance->event_sink) return;

  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"), flutter::EncodableValue("completed")}};
  flutter::EncodableValue event(encodables);
  instance->event_sink->Success(event);
}

void VideoPlayerPlugin::SendIsPlayingStateUpdate(int64_t texture_id,
                                                 bool is_playing) {
  const auto instance = FindPlayer(texture_id);
  if (!instance) return;
  std::lock_guard<std::mutex> guard(instance->error_delivery_mutex);
  if (!instance->event_sink) return;

  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"),
       flutter::EncodableValue("isPlayingStateUpdate")},
      {flutter::EncodableValue("isPlaying"),
       flutter::EncodableValue(is_playing)}};
  flutter::EncodableValue event(encodables);
  instance->event_sink->Success(event);
}

void VideoPlayerPlugin::SendErrorEventMessage(int64_t texture_id,
                                              const std::string& message) {
  const auto instance = FindPlayer(texture_id);
  if (!instance) return;
  // Init can fail before Dart subscribes. Retain errors until it attaches.
  instance->DeliverError(message);
}

std::shared_ptr<VideoPlayerPlugin::FlutterVideoPlayer>
VideoPlayerPlugin::FindPlayer(int64_t texture_id) {
  std::lock_guard<std::mutex> guard(players_mutex_);
  const auto it = players_.find(texture_id);
  return it == players_.end() ? nullptr : it->second;
}

void VideoPlayerPlugin::StartTextureDiagnostics(
    const std::shared_ptr<FlutterVideoPlayer>& player) {
#ifdef SOATV_TEXTURE_DIAG_DEFAULT
  const bool enabled_by_build = true;
#else
  const bool enabled_by_build = false;
#endif
  const char* enabled = std::getenv("SOATV_TEXTURE_DIAG");
  const bool enabled_at_runtime =
      enabled && ((enabled[0] == '1' && enabled[1] == '\0') ||
                  (enabled[0] == 't' && enabled[1] == 'r' &&
                   enabled[2] == 'u' && enabled[3] == 'e' &&
                   enabled[4] == '\0'));
  if (!enabled_by_build && !enabled_at_runtime) {
    return;
  }

  player->texture_diagnostics_enabled = true;
  std::cout << "TEXTURE_DIAG enabled texture=" << player->texture_id
            << " interval_ms=1000 path="
#ifdef USE_EGL_IMAGE_DMABUF
            << "egl"
#else
            << "pixel_buffer"
#endif
            << std::endl;
  player->texture_diagnostics_thread = std::thread(
      [this, player]() { ReportTextureDiagnostics(player); });
}

void VideoPlayerPlugin::StopTextureDiagnostics(
    const std::shared_ptr<FlutterVideoPlayer>& player) {
  if (!player->texture_diagnostics_thread.joinable()) return;
  player->texture_diagnostics_stopping.store(true, std::memory_order_release);
  player->texture_diagnostics_wait_cv.notify_all();
  if (player->texture_diagnostics_thread.get_id() !=
      std::this_thread::get_id()) {
    player->texture_diagnostics_thread.join();
  }
}

void VideoPlayerPlugin::ReportTextureDiagnostics(
    const std::shared_ptr<FlutterVideoPlayer>& player) {
  auto& stats = player->texture_diagnostics;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(player->texture_diagnostics_wait_mutex);
      player->texture_diagnostics_wait_cv.wait_for(
          lock, std::chrono::seconds(1), [player]() {
            return player->texture_diagnostics_stopping.load(
                std::memory_order_acquire);
          });
    }
    if (player->texture_diagnostics_stopping.load(std::memory_order_acquire)) {
      return;
    }

    const uint64_t decoded =
        stats.decoded_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t marked =
        stats.texture_marked.exchange(0, std::memory_order_relaxed);
    const uint64_t coalesced =
        stats.texture_coalesced.exchange(0, std::memory_order_relaxed);
    const uint64_t mark_failed =
        stats.texture_mark_failed.exchange(0, std::memory_order_relaxed);
    const uint64_t callbacks =
        stats.texture_callbacks.exchange(0, std::memory_order_relaxed);
    const uint64_t returned =
        stats.texture_returned.exchange(0, std::memory_order_relaxed);
    const uint64_t lock_busy =
        stats.texture_lock_busy.exchange(0, std::memory_order_relaxed);
    const uint64_t no_player =
        stats.texture_no_player.exchange(0, std::memory_order_relaxed);
    const uint64_t no_frame =
        stats.texture_no_frame.exchange(0, std::memory_order_relaxed);
    // Subset of no_frame: the frame existed but a handoff held the player's
    // buffer lock. The rest of no_frame means no decoded frame yet.
    uint64_t frame_lock_busy = 0;
    if (std::shared_ptr<GstVideoPlayer> native = player->GetPlayer()) {
      frame_lock_busy = native->TakeFrameLockBusyCount();
    }
    const uint64_t releases =
        stats.buffer_releases.exchange(0, std::memory_order_relaxed);
    const uint64_t copy_time_ns =
        stats.copy_time_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t copy_samples =
        stats.copy_time_samples.exchange(0, std::memory_order_relaxed);
    const uint64_t copy_max_ns =
        stats.copy_time_max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t mark_to_callback_time_ns =
        stats.mark_to_callback_time_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t mark_to_callback_samples =
        stats.mark_to_callback_samples.exchange(0, std::memory_order_relaxed);
    const uint64_t mark_to_callback_max_ns =
        stats.mark_to_callback_max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t hold_time_ns =
        stats.buffer_hold_time_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t hold_samples =
        stats.buffer_hold_time_samples.exchange(0, std::memory_order_relaxed);
    const uint64_t hold_max_ns =
        stats.buffer_hold_time_max_ns.exchange(0, std::memory_order_relaxed);

    std::ostringstream line;
    line << "TEXTURE_DIAG texture=" << player->texture_id
         << " interval_ms=1000 decoded_fps=" << decoded << " marked=" << marked
         << " coalesced=" << coalesced << " mark_failed=" << mark_failed
         << " callbacks=" << callbacks << " returned=" << returned
         << " lock_busy=" << lock_busy << " no_player=" << no_player
         << " no_frame=" << no_frame
         << " frame_lock_busy=" << frame_lock_busy << " releases=" << releases;
    if (copy_samples > 0) {
      line << " copy_avg_ms="
           << (static_cast<double>(copy_time_ns) / copy_samples / 1000000.0)
           << " copy_max_ms=" << (static_cast<double>(copy_max_ns) / 1000000.0);
    }
    if (mark_to_callback_samples > 0) {
      line << " mark_to_callback_avg_ms="
           << (static_cast<double>(mark_to_callback_time_ns) /
               mark_to_callback_samples / 1000000.0)
           << " mark_to_callback_max_ms="
           << (static_cast<double>(mark_to_callback_max_ns) / 1000000.0);
    }
    if (hold_samples > 0) {
      line << " buffer_hold_avg_ms="
           << (static_cast<double>(hold_time_ns) / hold_samples / 1000000.0)
           << " buffer_hold_max_ms="
           << (static_cast<double>(hold_max_ns) / 1000000.0);
    }
    std::cout << line.str() << std::endl;
  }
}

std::vector<int64_t> VideoPlayerPlugin::PlayerIds() {
  std::lock_guard<std::mutex> guard(players_mutex_);
  std::vector<int64_t> texture_ids;
  texture_ids.reserve(players_.size());
  for (const auto& entry : players_) {
    texture_ids.push_back(entry.first);
  }
  return texture_ids;
}

void VideoPlayerPlugin::DisposePlayer(int64_t texture_id) {
  std::shared_ptr<FlutterVideoPlayer> player;
  {
    std::lock_guard<std::mutex> guard(players_mutex_);
    auto it = players_.find(texture_id);
    if (it == players_.end()) return;
    player = it->second;
    players_.erase(it);
  }

  // Init() runs asynchronously; finish it before releasing the Gst player.
  if (player->initialization_thread.joinable() &&
      player->initialization_thread.get_id() != std::this_thread::get_id()) {
    player->initialization_thread.join();
  }
  StopTextureDiagnostics(player);

  // Detach the Dart-facing channels (platform thread — safe).
  {
    std::lock_guard<std::mutex> guard(player->error_delivery_mutex);
    player->event_sink = nullptr;
    player->pending_error.clear();
  }
  if (player->event_channel) {
    player->event_channel->SetStreamHandler(nullptr);
  }

  // Free the ALSA device and decoder before dispose() returns to Dart, so the
  // next player (video or audio) never finds plughw busy. ClearPlayer() below
  // only drops this map's reference; the destructor may run later on the
  // raster thread if a frame copy still holds the native player.
  if (std::shared_ptr<GstVideoPlayer> native = player->GetPlayer()) {
    native->ReleasePipeline();
  }

  // Tear down the GstVideoPlayer now (stops the pipeline and frees its
  // CMA-backed buffers, which must not linger across reconnects) but UNDER
  // buffer_mutex: if the raster thread is mid-copy, the copy callback holds the
  // lock until the engine finishes reading the frame (release_callback), so we
  // block here rather than freeing the frame out from under glTexImage2D.
  // Nulling `player` also makes any copy that starts afterwards bail at once.
  {
    std::lock_guard<std::mutex> guard(player->buffer_mutex);
    player->ClearPlayer();
  }

  // DEFERRED UNREGISTER (task #38, 2026-08-13). Do NOT call
  // texture_registrar_->UnregisterTexture(texture_id) here. Two crashes
  // proven via gdb core dump on 2026-08-10 (rapid reconnect churn) and
  // 2026-08-13 (FAITH TV, single reconnect after 1h stable playback) both
  // segfaulted inside flutter::ExternalTexturePixelBuffer::CopyPixelBuffer,
  // one via null-deref of a corrupted vtable, the other via blr on a
  // vtable slot pointing at unmapped memory. Both are use-after-free of
  // the ENGINE's ExternalTexturePixelBuffer object.
  //
  // The synchronous UnregisterTexture(int64_t) tears down that engine
  // object BEFORE the raster thread stops holding references from
  // in-flight render passes. Our existing buffer_mutex + retired_players_
  // pattern protects OUR FlutterVideoPlayer shell but NOT the engine's
  // texture wrapper. Deferring UnregisterTexture by the same 5 s window
  // keeps the engine's wrapper alive long enough for any raster-thread
  // reference to drain. During that window our callback returns nullptr
  // (player is null), so engine renders black for that texture — same
  // behavior as before, just with a live wrapper object.
  //
  // 2026-08-13 FAITH TV crash was triggered by ONE reconnect (not the 8+
  // reconnect churn of 2026-08-10) — the RepaintBoundary snapshot from
  // silent-reconnect UX (PR #37) held an extra texture reference during
  // dispose. Deferring here breaks that race regardless of trigger.
  ReapRetiredPlayers();
  {
    std::lock_guard<std::mutex> guard(players_mutex_);
    retired_players_.emplace_back(std::chrono::steady_clock::now(),
                                  std::move(player));
  }
}

void VideoPlayerPlugin::ReapRetiredPlayers() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<FlutterVideoPlayer>> expired;
  {
    std::lock_guard<std::mutex> guard(players_mutex_);
    for (auto it = retired_players_.begin(); it != retired_players_.end();) {
      // 5 s is far longer than the few frames the engine needs to stop invoking
      // an unregistered texture's callback, so the shell is safe to free.
      if (now - it->first > std::chrono::seconds(5)) {
        // Deferred UnregisterTexture (task #38). This is the actual call to
        // the engine that tears down its ExternalTexturePixelBuffer wrapper.
        // Delayed 5 s past DisposePlayer so raster-thread references from
        // in-flight render passes have drained.
        expired.push_back(it->second);
        it = retired_players_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& player : expired) {
    texture_registrar_->UnregisterTexture(player->texture_id);
  }
}

flutter::EncodableValue VideoPlayerPlugin::WrapError(
    const std::string& message, const std::string& code,
    const std::string& details) {
  flutter::EncodableMap map = {
      {flutter::EncodableValue("message"), flutter::EncodableValue(message)},
      {flutter::EncodableValue("code"), flutter::EncodableValue(code)},
      {flutter::EncodableValue("details"), flutter::EncodableValue(details)}};
  return flutter::EncodableValue(map);
}

const std::string VideoPlayerPlugin::GetExecutableDirectory() {
  static char buf[1024] = {};
  readlink("/proc/self/exe", buf, sizeof(buf) - 1);

  std::string exe_path = std::string(buf);
  const int slash_pos = exe_path.find_last_of('/');
  return exe_path.substr(0, slash_pos);
}

}  // namespace

void VideoPlayerElinuxPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  VideoPlayerPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar));
}
