#ifndef PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_LOGGING_H_
#define PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_LOGGING_H_

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>

namespace video_player_elinux {

class TimestampedLogBuffer : public std::streambuf {
 public:
  explicit TimestampedLogBuffer(std::streambuf* target, const std::string& path)
      : target_(target), log_file_(path, std::ios::app) {}

 protected:
  int overflow(int ch) override {
    if (ch == traits_type::eof()) {
      return traits_type::not_eof(ch);
    }

    const char c = static_cast<char>(ch);
    if (c == '\n') {
      FlushBuffer();
      return ch;
    }

    buffer_ << c;
    return ch;
  }

  std::streamsize xsputn(const char* s, std::streamsize n) override {
    if (n <= 0) {
      return 0;
    }

    buffer_.write(s, n);
    const std::string content = buffer_.str();
    buffer_.str("");
    buffer_.clear();

    std::string pending;
    pending.reserve(content.size());
    for (char ch : content) {
      if (ch == '\n') {
        FlushLine(pending);
        pending.clear();
      } else {
        pending.push_back(ch);
      }
    }

    if (!pending.empty()) {
      buffer_ << pending;
    }

    return n;
  }

  int sync() override {
    if (!buffer_.str().empty()) {
      FlushBuffer();
    }
    if (target_) {
      target_->pubsync();
    }
    if (log_file_.is_open()) {
      log_file_.flush();
    }
    return 0;
  }

 private:
  void FlushBuffer() {
    if (!buffer_.str().empty()) {
      FlushLine(buffer_.str());
      buffer_.str("");
      buffer_.clear();
    }
  }

  void FlushLine(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!log_file_.is_open()) {
      return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream timestamped;
    timestamped << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
    const std::string line = "[" + timestamped.str() + "] " + message;

    if (target_) {
      target_->sputn(line.c_str(), static_cast<std::streamsize>(line.size()));
      target_->sputc('\n');
    }
    log_file_ << line << '\n';
    log_file_.flush();
  }

  std::streambuf* target_ = nullptr;
  std::ofstream log_file_;
  std::ostringstream buffer_;
  std::mutex mutex_;
};

inline bool InitTimestampedLogging(const std::string& path = "/tmp/soatv.log") {
  static std::once_flag once;
  static std::streambuf* original_cout = nullptr;
  static std::streambuf* original_cerr = nullptr;
  static TimestampedLogBuffer* cout_buffer = nullptr;
  static TimestampedLogBuffer* cerr_buffer = nullptr;

  std::call_once(once, [&]() {
    original_cout = std::cout.rdbuf();
    original_cerr = std::cerr.rdbuf();
    cout_buffer = new TimestampedLogBuffer(original_cout, path);
    cerr_buffer = new TimestampedLogBuffer(original_cerr, path);
    std::cout.rdbuf(cout_buffer);
    std::cerr.rdbuf(cerr_buffer);
  });

  return true;
}

}  // namespace video_player_elinux

#endif  // PACKAGES_VIDEO_PLAYER_VIDEO_PLAYER_ELINUX_LOGGING_H_
