#include <syncbox/fs/debounce.hpp>

#include <utility>

namespace syncbox::fs {

Debouncer::Debouncer(std::chrono::milliseconds quiet_period, Callback callback)
    : quiet_period_(quiet_period), callback_(std::move(callback)) {
  thread_ = std::thread([this] { run(); });
}

Debouncer::~Debouncer() {
  stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Debouncer::notify() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = true;
    last_notification_ = std::chrono::steady_clock::now();
  }
  condition_.notify_one();
}

void Debouncer::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
}

void Debouncer::run() {
  std::unique_lock<std::mutex> lock(mutex_);

  while (!stopping_) {
    condition_.wait(lock, [this] { return pending_ || stopping_; });
    if (stopping_) {
      return;
    }

    while (!stopping_ && pending_) {
      const auto deadline = last_notification_ + quiet_period_;
      if (condition_.wait_until(lock, deadline) == std::cv_status::timeout) {
        break;
      }
    }

    if (stopping_) {
      return;
    }
    if (!pending_) {
      continue;
    }

    pending_ = false;
    lock.unlock();
    callback_();
    lock.lock();
  }
}

}  // namespace syncbox::fs
