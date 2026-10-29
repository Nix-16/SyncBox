#pragma once

#include <syncbox/core/version_model.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace syncbox::net {

// Blocking WebSocket client with reconnect. run() owns the I/O thread; stop()
// may be called from another thread. Change notifications are advisory: the
// callback normally just wakes the sync engine.
class WsClient {
 public:
  using ChangeCallback = std::function<void(Seq)>;

  WsClient(std::string base_url, std::string space_id, std::string token,
           ChangeCallback on_change);
  ~WsClient();

  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  void run();
  void stop();

  [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout);

 private:
  void wait_before_reconnect();

  std::string base_url_;
  std::string host_;
  std::string port_;
  std::string space_id_;
  std::string token_;
  ChangeCallback on_change_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::shared_ptr<class IoContextHolder> io_context_;
  bool connected_ = false;
  bool stopping_ = false;
};

}  // namespace syncbox::net
