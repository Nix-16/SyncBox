#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace syncbox::fs {

// Suppresses watcher events caused by the client's own writes. Keys are
// path_key values (NFC + casefold) from core/path.
class WriteSuppression {
 public:
  using Clock = std::chrono::steady_clock;

  void suppress(std::string_view path_key, std::chrono::milliseconds duration);
  [[nodiscard]] bool is_suppressed(std::string_view path_key);
  void clear();

 private:
  struct Deadline {
    Clock::time_point until;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, Deadline> entries_;
};

}  // namespace syncbox::fs
