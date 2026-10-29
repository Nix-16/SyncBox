#pragma once

#include <syncbox/client/engine.hpp>
#include <syncbox/core/result.hpp>
#include <syncbox/fs/debounce.hpp>
#include <syncbox/fs/watcher.hpp>
#include <syncbox/net/ws_client.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace syncbox::client {

// Continuous synchronization: filesystem events (debounced) plus a periodic
// poll drive SyncEngine::run_once() on a worker thread. Errors are retained in
// last_error() and retried on the next wake-up.
class SyncDaemon final : private fs::WatchSink {
 public:
  SyncDaemon(EngineConfig config, ClientStore& store,
             std::chrono::milliseconds poll_interval = std::chrono::seconds(30));
  ~SyncDaemon() override;

  SyncDaemon(const SyncDaemon&) = delete;
  SyncDaemon& operator=(const SyncDaemon&) = delete;

  [[nodiscard]] Result<void> start();
  void stop();

  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout);

 private:
  void on_event(const fs::WatchEvent& event) override;
  void on_error(std::string_view message) override;
  void request_sync();
  void worker();

  EngineConfig config_;
  ClientStore& store_;
  std::chrono::milliseconds poll_interval_;
  SyncEngine engine_;

  std::unique_ptr<fs::Watcher> watcher_;
  std::unique_ptr<fs::Debouncer> debouncer_;
  std::unique_ptr<net::WsClient> ws_client_;
  std::thread ws_thread_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool pending_ = false;
  bool stopping_ = false;
  std::string last_error_;
  std::thread worker_;
};

}  // namespace syncbox::client
