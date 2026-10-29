#include <syncbox/client/daemon.hpp>

#include <syncbox/common/time.hpp>

#include <utility>

namespace syncbox::client {

SyncDaemon::SyncDaemon(EngineConfig config, ClientStore& store,
                       std::chrono::milliseconds poll_interval)
    : config_(std::move(config)), store_(store), poll_interval_(poll_interval),
      engine_(config_, store_) {}

SyncDaemon::~SyncDaemon() {
  stop();
}

Result<void> SyncDaemon::start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return make_error(ErrorCode::Cancelled, "daemon has been stopped");
    }
    pending_ = true;
  }

  debouncer_ = std::make_unique<fs::Debouncer>(
      std::chrono::milliseconds(300), [this] { request_sync(); });

  auto watcher = fs::start_watcher(config_.root, *this);
  if (!watcher) {
    return watcher.error();
  }
  watcher_ = std::move(watcher.value());

  if (config_.server_url.rfind("http://", 0) == 0) {
    ws_client_ = std::make_unique<net::WsClient>(
        config_.server_url, config_.space_id, config_.device_token,
        [this](Seq) { request_sync(); });
    ws_thread_ = std::thread([this] { ws_client_->run(); });
  }

  worker_ = std::thread([this] { worker(); });
  return Result<void>{};
}

void SyncDaemon::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  condition_.notify_all();

  if (ws_client_) {
    ws_client_->stop();
  }
  if (ws_thread_.joinable()) {
    ws_thread_.join();
  }
  if (debouncer_) {
    debouncer_->stop();
  }
  if (watcher_) {
    watcher_->stop();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  ws_client_.reset();
  debouncer_.reset();
  watcher_.reset();
}

bool SyncDaemon::wait_connected(std::chrono::milliseconds timeout) {
  return ws_client_ != nullptr && ws_client_->wait_connected(timeout);
}

std::string SyncDaemon::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

void SyncDaemon::on_event(const fs::WatchEvent&) {
  if (debouncer_) {
    debouncer_->notify();
  }
}

void SyncDaemon::on_error(std::string_view message) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_error_ = std::string(message);
}

void SyncDaemon::request_sync() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = true;
  }
  condition_.notify_all();
}

void SyncDaemon::worker() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait_for(lock, poll_interval_,
                          [this] { return pending_ || stopping_; });
      if (stopping_) {
        return;
      }
      pending_ = false;
    }

    auto result = engine_.run_once();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result) {
      last_error_ = std::string(to_string(result.error().code)) + ": " +
                    result.error().message;
    } else {
      last_error_.clear();
    }
  }
}

}  // namespace syncbox::client
