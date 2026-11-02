#include <syncbox/server/rate_limiter.hpp>

#include <algorithm>
#include <utility>

namespace syncbox::server {

RateLimiter::RateLimiter(std::chrono::milliseconds window) : window_(window) {}

bool RateLimiter::allow(std::string_view key, int limit) {
  if (limit <= 0) {
    return true;
  }

  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  Bucket& bucket = buckets_[std::string(key)];
  if (!bucket.initialized) {
    bucket.tokens = static_cast<double>(limit);
    bucket.last = now;
    bucket.initialized = true;
  } else {
    const auto elapsed = std::chrono::duration<double>(now - bucket.last).count();
    const double window_seconds =
        std::max(0.001, std::chrono::duration<double>(window_).count());
    bucket.tokens = std::min(static_cast<double>(limit),
                             bucket.tokens + elapsed * static_cast<double>(limit) /
                                                 window_seconds);
    bucket.last = now;
  }

  if (bucket.tokens < 1.0) {
    return false;
  }
  bucket.tokens -= 1.0;
  return true;
}

void RateLimiter::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  buckets_.clear();
}

}  // namespace syncbox::server
