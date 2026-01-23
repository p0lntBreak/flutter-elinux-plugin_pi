// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_MESSAGES_CREATE_MESSAGE_H_
#define PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_MESSAGES_CREATE_MESSAGE_H_

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <map>

class CreateMessage {
 public:
  CreateMessage() = default;
  ~CreateMessage() = default;

  // Prevent copying.
  CreateMessage(CreateMessage const&) = default;
  CreateMessage& operator=(CreateMessage const&) = default;

  void SetAsset(const std::string& asset) { asset_ = asset; }

  std::string GetAsset() const { return asset_; }

  void SetUri(const std::string& uri) { uri_ = uri; }

  std::string GetUri() const { return uri_; }

  void SetPackageName(const std::string& packageName) {
    package_name_ = packageName;
  }

  std::string GetPackageName() const { return package_name_; }

  void SetFormatHint(const std::string& formatHint) {
    format_hint_ = formatHint;
  }

  std::string GetFormatHint() const { return format_hint_; }

  // ADD: HTTP Headers support
  void SetHttpHeaders(const std::map<std::string, std::string>& headers) {
    http_headers_ = headers;
  }

  std::map<std::string, std::string> GetHttpHeaders() const {
    return http_headers_;
  }

  flutter::EncodableValue ToMap() {
    flutter::EncodableMap map = {
        {flutter::EncodableValue("asset"), flutter::EncodableValue(asset_)},
        {flutter::EncodableValue("uri"), flutter::EncodableValue(uri_)},
        {flutter::EncodableValue("packageName"),
         flutter::EncodableValue(package_name_)},
        {flutter::EncodableValue("formatHint"),
         flutter::EncodableValue(format_hint_)}};
    
    // ADD: Include httpHeaders in the map
    if (!http_headers_.empty()) {
      flutter::EncodableMap headers_map;
      for (const auto& [key, value] : http_headers_) {
        headers_map[flutter::EncodableValue(key)] = flutter::EncodableValue(value);
      }
      map[flutter::EncodableValue("httpHeaders")] = flutter::EncodableValue(headers_map);
    }
    
    return flutter::EncodableValue(map);
  }

  static CreateMessage FromMap(const flutter::EncodableValue& value) {
    CreateMessage message;
    if (std::holds_alternative<flutter::EncodableMap>(value)) {
      auto map = std::get<flutter::EncodableMap>(value);

      flutter::EncodableValue& asset = map[flutter::EncodableValue("asset")];
      if (std::holds_alternative<std::string>(asset)) {
        message.SetAsset(std::get<std::string>(asset));
      }

      flutter::EncodableValue& uri = map[flutter::EncodableValue("uri")];
      if (std::holds_alternative<std::string>(uri)) {
        message.SetUri(std::get<std::string>(uri));
      }

      flutter::EncodableValue& packageName =
          map[flutter::EncodableValue("packageName")];
      if (std::holds_alternative<std::string>(packageName)) {
        message.SetPackageName(std::get<std::string>(packageName));
      }

      flutter::EncodableValue& formatHint =
          map[flutter::EncodableValue("formatHint")];
      if (std::holds_alternative<std::string>(formatHint)) {
        message.SetFormatHint(std::get<std::string>(formatHint));
      }

      // ADD: Parse httpHeaders from the map
      auto headers_it = map.find(flutter::EncodableValue("httpHeaders"));
      if (headers_it != map.end() && 
          std::holds_alternative<flutter::EncodableMap>(headers_it->second)) {
        auto headers_map = std::get<flutter::EncodableMap>(headers_it->second);
        std::map<std::string, std::string> headers;
        
        for (const auto& [key, value] : headers_map) {
          if (std::holds_alternative<std::string>(key) && 
              std::holds_alternative<std::string>(value)) {
            headers[std::get<std::string>(key)] = std::get<std::string>(value);
          }
        }
        
        if (!headers.empty()) {
          message.SetHttpHeaders(headers);
        }
      }
    }

    return message;
  }

 private:
  std::string asset_;
  std::string uri_;
  std::string package_name_;
  std::string format_hint_;
  std::map<std::string, std::string> http_headers_;  // ADD THIS
};

#endif 
