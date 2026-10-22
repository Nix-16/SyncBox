#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace syncbox::fs {

// Coalesces a burst of notifications into one callback after the quiet
// period elapses. The callback runs on the debouncer's background thread.
class Debouncer {
 public:
  using Callback = std::function<void()>;

  Debouncer(std::chrono::milliseconds quiet_period, Callback callback);
  ~Debouncer();

  Debouncer(const Debouncer&) = delete;
  Debouncer& operator=(const Debouncer&) = delete;

  void notify();
  void stop();

 private:
  void run();

  std::chrono::milliseconds quiet_period_;
  Callback callback_;

  std::mutex mutex_;
  std::condition_variable condition_;
  bool pending_ = false;
  bool stopping_ = false;
  std::chrono::steady_clock::time_point last_notification_{};
  std::thread thread_;
};

}  // namespace syncbox::fs
