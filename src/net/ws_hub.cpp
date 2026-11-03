#include <syncbox/net/ws_hub.hpp>

#include <algorithm>
#include <utility>

namespace syncbox::net {

void WsHub::add(const std::shared_ptr<WsConnection>& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.push_back(session);
}

void WsHub::remove(WsConnection* session) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [session](const std::weak_ptr<WsConnection>& entry) {
                         const auto shared = entry.lock();
                         return !shared || shared.get() == session;
                       }),
        sessions_.end());
  }
  empty_condition_.notify_all();
}

void WsHub::close_device(const std::string& device_id) {
  std::vector<std::shared_ptr<WsConnection>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto session = it->lock();
      if (!session) {
        it = sessions_.erase(it);
        continue;
      }
      if (session->device_id() == device_id) {
        targets.push_back(std::move(session));
      }
      ++it;
    }
  }
  for (auto& session : targets) {
    session->request_close();
  }
}

void WsHub::notify_change(const std::string& space_id, Seq seq) {
  std::vector<std::shared_ptr<WsConnection>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto session = it->lock();
      if (!session) {
        it = sessions_.erase(it);
        continue;
      }
      if (session->space_id() == space_id) {
        targets.push_back(std::move(session));
      }
      ++it;
    }
  }
  for (auto& session : targets) {
    session->send_change(seq);
  }
}

std::size_t WsHub::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const auto& entry : sessions_) {
    if (!entry.expired()) {
      ++count;
    }
  }
  return count;
}

bool WsHub::wait_empty(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return empty_condition_.wait_for(lock, timeout, [this] {
    for (const auto& entry : sessions_) {
      if (!entry.expired()) {
        return false;
      }
    }
    return true;
  });
}

}  // namespace syncbox::net
