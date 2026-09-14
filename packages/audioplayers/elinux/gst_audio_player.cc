// Copyright 2024 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_audio_player.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <glob.h>
#include <iostream>
#include <string>

// Pick the ALSA device associated with the connected HDMI connector. Keep
// this local to the audio player so the existing video-player routing remains
// unchanged.
static bool ReadFirstLine(const std::string& path, std::string* line) {
  std::ifstream f(path);
  return f && std::getline(f, *line);
}

static std::string DeviceForAlsaCard(int card) {
  char device[32];
  std::snprintf(device, sizeof(device), "plughw:%d,0", card);
  return device;
}

static bool FindAlsaCardById(const std::string& target_id, int* out_card) {
  for (int card = 0; card < 32; ++card) {
    char id_path[64];
    std::snprintf(id_path, sizeof(id_path), "/proc/asound/card%d/id", card);
    std::string id;
    if (ReadFirstLine(id_path, &id) && id == target_id) {
      *out_card = card;
      return true;
    }
  }
  return false;
}

static bool FindAnyHdmiAlsaCard(int* out_card, std::string* out_id) {
  for (int card = 0; card < 32; ++card) {
    char id_path[64];
    std::snprintf(id_path, sizeof(id_path), "/proc/asound/card%d/id", card);
    std::string id;
    if (ReadFirstLine(id_path, &id) && id.rfind("vc4hdmi", 0) == 0) {
      *out_card = card;
      *out_id = id;
      return true;
    }
  }
  return false;
}

static std::string PickAudioDevice() {
  const char* override_device = std::getenv("AUDIOPLAYERS_ELINUX_ALSA_DEVICE");
  if (override_device && override_device[0] != '\0') {
    std::cout << "PickAudioDevice (audio): using override "
              << override_device << std::endl;
    return override_device;
  }

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
      std::string status;
      if (ReadFirstLine(g.gl_pathv[i], &status) && status == "connected") {
        connected = true;
        break;
      }
    }
    globfree(&g);
    if (!connected) {
      continue;
    }

    char target[16];
    std::snprintf(target, sizeof(target), "vc4hdmi%d", hdmi_idx - 1);
    int card = -1;
    if (FindAlsaCardById(target, &card)) {
      std::string device = DeviceForAlsaCard(card);
      std::cout << "PickAudioDevice (audio): HDMI-A-" << hdmi_idx
                << " connected, ALSA card " << card << " (" << target
                << ") -> " << device << std::endl;
      return device;
    }

    std::cerr << "PickAudioDevice (audio): HDMI-A-" << hdmi_idx
              << " connected but no ALSA card matching '" << target
              << "' found" << std::endl;
  }

  int hdmi_card = -1;
  std::string hdmi_id;
  if (FindAnyHdmiAlsaCard(&hdmi_card, &hdmi_id)) {
    std::string device = DeviceForAlsaCard(hdmi_card);
    std::cout << "PickAudioDevice (audio): DRM did not report connected HDMI, "
              << "using ALSA HDMI card " << hdmi_card << " (" << hdmi_id
              << ") -> " << device << std::endl;
    return device;
  }

  std::cout << "PickAudioDevice (audio): no vc4hdmi ALSA card found, "
               "falling back to plughw:0,0"
            << std::endl;
  return "plughw:0,0";
}

GstAudioPlayer::GstAudioPlayer(
    const std::string &player_id,
    std::unique_ptr<AudioPlayerStreamHandler> handler)
    : player_id_(player_id),
    stream_handler_(std::move(handler)) {
  gst_.playbin = nullptr;
  gst_.bus = nullptr;
  gst_.source = nullptr;
  gst_.panorama = nullptr;
  gst_.audiobin = nullptr;
  gst_.audiosink = nullptr;
  gst_.panoramasinkpad = nullptr;

  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    Dispose();
    return;
  }
}

GstAudioPlayer::~GstAudioPlayer() {
  Stop();
  Dispose();
}

// static
void GstAudioPlayer::GstLibraryLoad() { gst_init(NULL, NULL); }

// static
void GstAudioPlayer::GstLibraryUnload() { gst_deinit(); }

// Creates a audio playbin.
// $ playbin uri=<file>
bool GstAudioPlayer::CreatePipeline() {
  gst_.playbin = gst_element_factory_make("playbin", "playbin");
  if (!gst_.playbin) {
    std::cerr << "Failed to create a playbin" << std::endl;
    return false;
  }

  // Setup stereo balance controller
  gst_.panorama = gst_element_factory_make("audiopanorama", "audiopanorama");
  if (gst_.panorama) {
    gst_.audiobin = gst_bin_new(NULL);
    gst_.audiosink = gst_element_factory_make("alsasink", "alsasink");
    if (!gst_.audiosink) {
      std::cerr << "Failed to create alsasink" << std::endl;
      return false;
    }

    const std::string audio_device = PickAudioDevice();
    g_object_set(G_OBJECT(gst_.audiosink), "device", audio_device.c_str(),
                 NULL);

    gst_bin_add_many(GST_BIN(gst_.audiobin), gst_.panorama, gst_.audiosink, NULL);
    gst_element_link(gst_.panorama, gst_.audiosink);

    GstPad* sinkpad = gst_element_get_static_pad(gst_.panorama, "sink");
    gst_.panoramasinkpad = gst_ghost_pad_new("sink", sinkpad);
    gst_element_add_pad(gst_.audiobin, gst_.panoramasinkpad);
    gst_object_unref(GST_OBJECT(sinkpad));

    g_object_set(G_OBJECT(gst_.playbin), "audio-sink", gst_.audiobin, NULL);
    g_object_set(G_OBJECT(gst_.panorama), "method", 1, NULL);
  }

  // Setup source options
  g_signal_connect(gst_.playbin, "source-setup",
                   G_CALLBACK(GstAudioPlayer::SourceSetup), &gst_.source);

  // Watch bus messages for one time events
  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.playbin));
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

  return true;
}

void GstAudioPlayer::UpdateAudioDevice() {
  if (!gst_.audiosink) {
    return;
  }

  const std::string audio_device = PickAudioDevice();
  std::cout << "UpdateAudioDevice (audio): setting device to " << audio_device
            << std::endl;
  g_object_set(G_OBJECT(gst_.audiosink), "device", audio_device.c_str(),
               NULL);
}

// static
void GstAudioPlayer::SourceSetup(GstElement* playbin,
                                 GstElement* source,
                                 GstElement** p_src) {
  // Allow sources from unencrypted / misconfigured connections
  if (g_object_class_find_property(
      G_OBJECT_GET_CLASS(source), "ssl-strict") != 0) {
    g_object_set(G_OBJECT(source), "ssl-strict", FALSE, NULL);
  }
}

std::string GstAudioPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) {
    return uri;
  }

  const auto* filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
  if (!filename_uri) {
    std::cerr << "Faild to open " << uri.c_str() << std::endl;
    return uri;
  }
  std::string result_uri(filename_uri);
  delete filename_uri;

  return result_uri;
}

void GstAudioPlayer::Resume() {
  if (!is_playing_) {
    is_playing_ = true;
  }

  if (!is_initialized_) {
    return;
  }

  if (gst_element_set_state(gst_.playbin, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Unable to set the pipeline to GST_STATE_PLAYING" << std::endl;
    return;
  }
  int64_t duration = GetDuration();
  stream_handler_->OnNotifyDuration(player_id_, duration);
}

void GstAudioPlayer::Play() {
  Seek(0);
  Resume();
}

void GstAudioPlayer::Pause() {
  if (is_playing_) {
    is_playing_ = false;
  }
  if (!is_initialized_) {
    return;
  }
  if (gst_element_set_state(gst_.playbin, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return;
  }
}

void GstAudioPlayer::Stop() {
  Pause();
  if (!is_initialized_) {
    return;
  }
  Seek(0);
}

void GstAudioPlayer::Seek(int64_t position) {
  if (!is_initialized_) {
    return;
  }
  auto nanosecond = position * 1000 * 1000;
  if (!gst_element_seek(
          gst_.playbin, playback_rate_, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
          GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek " << position << std::endl;
    return;
  }
  stream_handler_->OnNotifySeekCompleted(player_id_);
}

void GstAudioPlayer::SetSourceUrl(std::string url) {
  if (url_ != url) {
    url_ = url;

    // Update audio device in case HDMI was connected after app startup
    UpdateAudioDevice();

    // flush unhandled messeges
    gst_bus_set_flushing(gst_.bus, TRUE);
    gst_element_set_state(gst_.playbin, GST_STATE_NULL);
    is_playing_ = false;
    if (!url_.empty()) {
      g_object_set(GST_OBJECT(gst_.playbin), "uri", url_.c_str(), NULL);
      if (gst_.playbin->current_state != GST_STATE_READY) {
        GstStateChangeReturn ret =
            gst_element_set_state(gst_.playbin, GST_STATE_READY);
        if (ret == GST_STATE_CHANGE_FAILURE) {
          std::cerr <<
            "Unable to set the pipeline to GST_STATE_READY." << std::endl;
        }
      }
    }
    is_initialized_ = true;
  }
  stream_handler_->OnNotifyPrepared(player_id_, true);
}

void GstAudioPlayer::SetVolume(double volume) {
  if (volume > 1) {
    volume = 1;
  } else if (volume < 0) {
    volume = 0;
  }
  volume_ = volume;
  g_object_set(gst_.playbin, "volume", volume, NULL);
}

void GstAudioPlayer::SetBalance(double balance) {
  if (!gst_.panorama) {
    std::cerr << "Audiopanorama was not initialized" << std::endl;
    return;
  }

  if (balance > 1.0) {
    balance = 1.0;
  } else if (balance < -1.0) {
    balance = -1.0;
  }
  g_object_set(G_OBJECT(gst_.panorama), "panorama", balance, NULL);
}

void GstAudioPlayer::SetPlaybackRate(double playback_rate) {
  if (playback_rate <= 0) {
    std::cerr << "Rate " << playback_rate << " is not supported" << std::endl;
    return;
  }

  if (!is_initialized_) {
    return;
  }
  int64_t position = GetCurrentPosition();
  if (!gst_element_seek(gst_.playbin, playback_rate, GST_FORMAT_TIME,
                        GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                        position * GST_MSECOND, GST_SEEK_TYPE_SET,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to set playback rate to " << playback_rate
              << " (gst_element_seek failed)" << std::endl;
    return;
  }

  playback_rate_ = playback_rate;
}

void GstAudioPlayer::SetLooping(bool is_looping) {
  is_looping_ = is_looping;
}

int64_t GstAudioPlayer::GetDuration() {
  gint64 duration;
  if (!gst_element_query_duration(gst_.playbin, GST_FORMAT_TIME, &duration)) {
    return -1;
  }

  return duration /= GST_MSECOND;
}

int64_t GstAudioPlayer::GetCurrentPosition() {
  gint64 position = 0;
  if (!gst_element_query_position(gst_.playbin, GST_FORMAT_TIME, &position)) {
    return -1;
  }

  // TODO: We need to handle this code in the proper plase.
  // The VideoPlayer plugin doesn't have a main loop, so EOS message
  // received from GStreamer cannot be processed directly in a callback
  // function. This is because the event channel message of playback complettion
  // needs to be thrown in the main thread.
  if (is_completed_) {
    is_completed_ = false;
    if (is_looping_) {
      Play();
    }  else {
      stream_handler_->OnNotifyPlayCompleted(player_id_);
      Stop();
    }
    position = 0;
  }

  return position / GST_MSECOND;
}

void GstAudioPlayer::Release() {
  is_playing_ = false;
  is_initialized_ = false;
  url_.clear();

  GstState state;
  gst_element_get_state(gst_.playbin, &state, NULL, GST_CLOCK_TIME_NONE);
  if (state > GST_STATE_NULL) {
    gst_bus_set_flushing(gst_.bus, TRUE);
    gst_element_set_state(gst_.playbin, GST_STATE_NULL);
  }
}

void GstAudioPlayer::Dispose() {
  if (!gst_.playbin) {
    std::cerr << "Already disposed" << std::endl;
    return;
  }
  is_playing_ = false;
  is_initialized_ = false;
  url_.clear();

  if (gst_.bus) {
    gst_bus_set_flushing(gst_.bus, TRUE);
    gst_object_unref(GST_OBJECT(gst_.bus));
    gst_.bus = nullptr;
  }

  if (gst_.source) {
    gst_object_unref(GST_OBJECT(gst_.source));
    gst_.source = nullptr;
  }

  if (gst_.panorama) {
    gst_element_set_state(gst_.audiobin, GST_STATE_NULL);
    gst_element_remove_pad(gst_.audiobin, gst_.panoramasinkpad);
    gst_bin_remove(GST_BIN(gst_.audiobin), gst_.audiosink);
    gst_bin_remove(GST_BIN(gst_.audiobin), gst_.panorama);
    gst_.panorama = nullptr;
  }

  gst_.playbin = nullptr;
}

// static
GstBusSyncReply GstAudioPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  auto* self = reinterpret_cast<GstAudioPlayer*>(user_data);
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->gst_.playbin)) {
        GstState old_state, new_state;
        gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
        if (new_state == GST_STATE_READY) {
          if (gst_element_set_state(self->gst_.playbin, GST_STATE_PAUSED) ==
              GST_STATE_CHANGE_FAILURE) {
            g_printerr("Unable to set the pipeline from GST_STATE_READY "
                "to GST_STATE_PAUSED\n");
          }
        }
      }
      break;
    }
    case GST_MESSAGE_EOS:
      self->is_completed_ = true;
      break;
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

  gst_message_unref(message);

  return GST_BUS_DROP;
}
