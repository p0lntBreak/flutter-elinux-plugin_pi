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

#include <chrono>
#include <memory>
#include <mutex>
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
    texture_ids.reserve(players_.size());
    for (const auto& entry : players_) {
      texture_ids.push_back(entry.first);
    }
    for (const auto texture_id : texture_ids) {
      DisposePlayer(texture_id);
    }
    // Every GstVideoPlayer was destroyed synchronously inside DisposePlayer;
    // the retired shells hold no GStreamer objects, so drop them now, before
    // the library unload.
    retired_players_.clear();
    GstVideoPlayer::GstLibraryUnload();
  }

 private:
  struct FlutterVideoPlayer {
    int64_t texture_id;
    std::unique_ptr<GstVideoPlayer> player;
    std::unique_ptr<flutter::TextureVariant> texture;
    std::unique_ptr<FlutterDesktopPixelBuffer> buffer;
    // Serialises the pixel-buffer copy (and the engine's subsequent
    // glTexImage2D read, released via the buffer's release_callback) against
    // DisposePlayer() tearing down `player`. See DisposePlayer().
    std::mutex buffer_mutex;
#ifdef USE_EGL_IMAGE_DMABUF
    std::unique_ptr<FlutterDesktopEGLImage> egl_image;
#endif  // USE_EGL_IMAGE_DMABUF
    std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
        event_channel;
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink;
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
  void ReapRetiredPlayers();

  flutter::EncodableValue WrapError(const std::string& message,
                                    const std::string& code = std::string(),
                                    const std::string& details = std::string());

  const std::string GetExecutableDirectory();

  flutter::PluginRegistrar* plugin_registrar_;
  flutter::TextureRegistrar* texture_registrar_;
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
  std::vector<int64_t> texture_ids;
  texture_ids.reserve(players_.size());
  for (const auto& entry : players_) {
    texture_ids.push_back(entry.first);
  }
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
            if (!instance->player) {
              return nullptr;
            }
            instance->egl_image->width = instance->player->GetWidth();
            instance->egl_image->height = instance->player->GetHeight();
            instance->egl_image->egl_image =
                instance->player->GetEGLImage(egl_display, egl_context);
            return instance->egl_image.get();
          }));
#else
  instance->buffer = std::make_unique<FlutterDesktopPixelBuffer>();
  instance->texture =
      std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
          [instance = instance.get()](
              size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
            // Hold buffer_mutex across the whole copy AND the engine's later
            // glTexImage2D read: the engine reads the returned buffer after
            // this callback returns and signals completion via the buffer's
            // release_callback, so the lock is released there (or on an early
            // return here). DisposePlayer() takes the same lock before freeing
            // `player`, so the frame can't be freed mid-read; and the shell is
            // kept alive past dispose (retired_players_), so this never
            // dereferences freed memory even if it fires after dispose.
            instance->buffer_mutex.lock();
            if (!instance->player) {
              instance->buffer_mutex.unlock();
              return nullptr;
            }
            const uint8_t* frame = instance->player->GetFrameBuffer();
            if (!frame) {
              instance->buffer_mutex.unlock();
              return nullptr;
            }
            instance->buffer->width = instance->player->GetWidth();
            instance->buffer->height = instance->player->GetHeight();
            instance->buffer->buffer = frame;
            instance->buffer->release_callback = [](void* ctx) {
              static_cast<FlutterVideoPlayer*>(ctx)->buffer_mutex.unlock();
            };
            instance->buffer->release_context = instance;
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
          instance->event_sink = std::move(events);
          host->SendInitializedEventMessage(instance->texture_id);
          return nullptr;
        },
        [instance = instance.get()](const flutter::EncodableValue* arguments)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
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
        [texture_id, host = this]() {
          host->texture_registrar_->MarkTextureFrameAvailable(texture_id);
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
          if (host->players_.find(texture_id) != host->players_.end() &&
              host->players_[texture_id]->event_sink) {
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingStart")}};
            flutter::EncodableValue event(encodables);
            host->players_[texture_id]->event_sink->Success(event);
          }
        },
        // OnNotifyBufferingUpdate
        [texture_id, host = this](int percent) {
          if (host->players_.find(texture_id) != host->players_.end() &&
              host->players_[texture_id]->event_sink) {
            auto duration = host->players_[texture_id]->player->GetDuration();
            if (duration < 0) duration = 0;
            int64_t buffered_end = (duration * percent) / 100;

            flutter::EncodableList ranges = {
                flutter::EncodableValue(flutter::EncodableList{
                    flutter::EncodableValue(static_cast<int64_t>(0)),
                    flutter::EncodableValue(buffered_end)
                })
            };
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingUpdate")},
                {flutter::EncodableValue("values"),
                 flutter::EncodableValue(ranges)}};
            flutter::EncodableValue event(encodables);
            host->players_[texture_id]->event_sink->Success(event);
          }
        },
        // OnNotifyBufferingEnd
        [texture_id, host = this]() {
          if (host->players_.find(texture_id) != host->players_.end() &&
              host->players_[texture_id]->event_sink) {
            flutter::EncodableMap encodables = {
                {flutter::EncodableValue("event"),
                 flutter::EncodableValue("bufferingEnd")}};
            flutter::EncodableValue event(encodables);
            host->players_[texture_id]->event_sink->Success(event);
          }
        });

      
    instance->player =
        std::make_unique<GstVideoPlayer>(uri, std::move(player_handler));

      

    //Extract and apply HTTP headers dynamically
    const auto& http_headers = meta.GetHttpHeaders();
    if (!http_headers.empty()) {
      std::cout << "Received " << http_headers.size() << " HTTP headers from Flutter" << std::endl;
      
      // Log all headers
      for (const auto& [key, value] : http_headers) {
        std::cout << "  Header: " << key << " = " << value << std::endl;
      }
      
      // Pass ALL headers to the player
      std::cout << "Setting ALL HTTP headers on player" << std::endl;
      instance->player->SetAuthHeaders(http_headers);
    } else {
      std::cout << "No HTTP headers provided from Flutter" << std::endl;
    }
      
    players_[texture_id] = std::move(instance);
  }

flutter::EncodableMap value;
  TextureMessage result;

  bool ok = players_[texture_id]->player->Init();
  if (ok) {
    result.SetTextureId(texture_id);
    value.emplace(flutter::EncodableValue(kEncodableMapkeyResult),
                  result.ToMap());
  } else {
    // Prefer the specific error surfaced from Init() (e.g. NETWORK_TOO_SLOW:,
    // STREAM_UNAVAILABLE:) if one was recorded — the Dart side branches on
    // those prefixes to render distinct UIs. Fall back to the generic
    // texture-id message if nothing was recorded (e.g. a very early failure
    // before any NotifyError path fired).
    std::string specific = players_[texture_id]->player->GetLastError();
    auto error_message = !specific.empty()
        ? specific
        : ("Failed to initialize the player with texture id: " +
           std::to_string(texture_id));
    value.emplace(flutter::EncodableValue(kEncodableMapkeyError),
                  flutter::EncodableValue(WrapError(error_message)));
    // Init() failed. The texture was already registered and this entry
    // already inserted into players_ above, but Dart's create() never
    // returns a textureId on this branch (the Future rejects with
    // PlatformException) — so Dart has no id to pass to dispose() later.
    // Clean up here or the texture + player leak on every failed Init().
    // DisposePlayer() erases from players_ itself.
    DisposePlayer(texture_id);
  }
  reply(flutter::EncodableValue(value));
}

void VideoPlayerPlugin::HandleDisposeMethodCall(
    const flutter::EncodableValue& message,
    flutter::MessageReply<flutter::EncodableValue> reply) {
  auto parameter = TextureMessage::FromMap(message);
  const auto texture_id = parameter.GetTextureId();
  flutter::EncodableMap result;

  if (players_.find(texture_id) != players_.end()) {
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

  if (players_.find(texture_id) != players_.end()) {
    // Diagnostic: a pipeline stuck in PAUSED traces back to whoever sent this.
    std::cout << "PAUSE method call from Dart for texture " << texture_id
              << std::endl;
    players_[texture_id]->player->Pause();
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

  if (players_.find(texture_id) != players_.end()) {
    players_[texture_id]->player->Play();
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

  if (players_.find(texture_id) != players_.end()) {
    players_[texture_id]->player->SetAutoRepeat(parameter.GetIsLooping());
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

  if (players_.find(texture_id) != players_.end()) {
    players_[texture_id]->player->SetVolume(parameter.GetVolume());
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

  if (players_.find(texture_id) != players_.end()) {
    auto position = players_[texture_id]->player->GetCurrentPosition();
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

  if (players_.find(texture_id) != players_.end()) {
    players_[texture_id]->player->SetPlaybackRate(parameter.GetSpeed());
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

  if (players_.find(texture_id) != players_.end()) {
    players_[texture_id]->player->SetSeek(parameter.GetPosition());
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

void VideoPlayerPlugin::SendInitializedEventMessage(int64_t texture_id) {
  if (players_.find(texture_id) == players_.end() ||
      !players_[texture_id]->event_sink) {
    return;
  }

  auto duration = players_[texture_id]->player->GetDuration();
  auto width = players_[texture_id]->player->GetWidth();
  auto height = players_[texture_id]->player->GetHeight();
  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"),
       flutter::EncodableValue("initialized")},
      {flutter::EncodableValue("duration"), flutter::EncodableValue(duration)},
      {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
      {flutter::EncodableValue("height"), flutter::EncodableValue(height)}};
  flutter::EncodableValue event(encodables);
  players_[texture_id]->event_sink->Success(event);
}

void VideoPlayerPlugin::SendPlayCompletedEventMessage(int64_t texture_id) {
  if (players_.find(texture_id) == players_.end() ||
      !players_[texture_id]->event_sink) {
    return;
  }

  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"), flutter::EncodableValue("completed")}};
  flutter::EncodableValue event(encodables);
  players_[texture_id]->event_sink->Success(event);
}

void VideoPlayerPlugin::SendIsPlayingStateUpdate(int64_t texture_id,
                                                 bool is_playing) {
  if (players_.find(texture_id) == players_.end() ||
      !players_[texture_id]->event_sink) {
    return;
  }

  flutter::EncodableMap encodables = {
      {flutter::EncodableValue("event"),
       flutter::EncodableValue("isPlayingStateUpdate")},
      {flutter::EncodableValue("isPlaying"),
       flutter::EncodableValue(is_playing)}};
  flutter::EncodableValue event(encodables);
  players_[texture_id]->event_sink->Success(event);
}

void VideoPlayerPlugin::SendErrorEventMessage(int64_t texture_id,
                                              const std::string& message) {
  if (players_.find(texture_id) == players_.end() ||
      !players_[texture_id]->event_sink) {
    return;
  }
  players_[texture_id]->event_sink->Error("VideoError", message);
}

void VideoPlayerPlugin::DisposePlayer(int64_t texture_id) {
  auto it = players_.find(texture_id);
  if (it == players_.end()) {
    return;
  }
  std::shared_ptr<FlutterVideoPlayer> player = it->second;
  players_.erase(it);

  // Detach the Dart-facing channels (platform thread — safe).
  player->event_sink = nullptr;
  if (player->event_channel) {
    player->event_channel->SetStreamHandler(nullptr);
  }

  // Ask the engine to stop sampling this texture. This client wrapper only
  // exposes the deprecated synchronous UnregisterTexture(int64_t), which
  // returns BEFORE the raster thread has necessarily finished a copy in flight
  // (or one it is about to start), so it does not by itself make teardown safe.
  texture_registrar_->UnregisterTexture(texture_id);

  // Tear down the GstVideoPlayer now (stops the pipeline and frees its
  // CMA-backed buffers, which must not linger across reconnects) but UNDER
  // buffer_mutex: if the raster thread is mid-copy, the copy callback holds the
  // lock until the engine finishes reading the frame (release_callback), so we
  // block here rather than freeing the frame out from under glTexImage2D.
  // Nulling `player` also makes any copy that starts afterwards bail at once.
  {
    std::lock_guard<std::mutex> guard(player->buffer_mutex);
    player->player = nullptr;
  }

  // Do NOT destroy the FlutterVideoPlayer shell here. A copy the raster thread
  // already scheduled can still fire after UnregisterTexture() returns, and it
  // dereferences this object (and its buffer_mutex). Keep the shell alive —
  // `player` is now null, so the callback returns nullptr immediately — and
  // reclaim it once the raster thread has certainly drained. This closes the
  // use-after-free that segfaulted on reconnect teardown.
  ReapRetiredPlayers();
  retired_players_.emplace_back(std::chrono::steady_clock::now(),
                                std::move(player));
}

void VideoPlayerPlugin::ReapRetiredPlayers() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = retired_players_.begin(); it != retired_players_.end();) {
    // 5 s is far longer than the few frames the engine needs to stop invoking
    // an unregistered texture's callback, so the shell is safe to free.
    if (now - it->first > std::chrono::seconds(5)) {
      it = retired_players_.erase(it);
    } else {
      ++it;
    }
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
