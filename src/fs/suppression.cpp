#include <syncbox/fs/suppression.hpp>

namespace syncbox::fs {

void WriteSuppression::suppress(std::string_view path_key, std::chrono::milliseconds duration) {
  const auto deadline = Clock::now() + duration;
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[std::string(path_key)] = Deadline{deadline};
}

bool WriteSuppression::is_suppressed(std::string_view path_key) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  const auto it = entries_.find(std::string(path_key));
  if (it == entries_.end()) {
    return false;
  }
  if (it->second.until <= now) {
    entries_.erase(it);
    return false;
  }
  return true;
}

void WriteSuppression::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

}  // namespace syncbox::fs
