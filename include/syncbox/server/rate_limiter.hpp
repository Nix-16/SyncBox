#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace syncbox::server {

// Per-device request limits (PROTOCOL.md section 6). Values are per minute.
struct RateLimitConfig {
  int ops_per_minute = 60;
  int chunk_puts_per_minute = 600;
  int changes_per_minute = 120;
};

// Token-bucket limiter keyed by an opaque string (device token hash +
// category). Thread-safe; used by the API layer.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  explicit RateLimiter(std::chrono::milliseconds window = std::chrono::minutes(1));

  // Returns true when the request may proceed. limit <= 0 disables limiting.
  [[nodiscard]] bool allow(std::string_view key, int limit);

  void reset();

 private:
  struct Bucket {
    double tokens = 0.0;
    Clock::time_point last{};
    bool initialized = false;
  };

  std::mutex mutex_;
  std::chrono::milliseconds window_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace syncbox::server
