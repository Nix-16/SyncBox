#include <syncbox/fs/watcher.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace syncbox::fs {

namespace {

constexpr std::uint32_t kWatchMask =
    IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_DELETE_SELF | IN_MOVE_SELF | IN_EXCL_UNLINK;

constexpr std::size_t kEventBufferSize = 64 * 1024;
constexpr std::chrono::seconds kMovePairingWindow{2};

struct PendingMove {
  std::string path;
  std::chrono::steady_clock::time_point seen;
};

class InotifyWatcher final : public Watcher {
 public:
  InotifyWatcher(std::filesystem::path root, WatchSink& sink, int inotify_fd, int wake_fd)
      : root_(std::move(root)), sink_(sink), inotify_fd_(inotify_fd), wake_fd_(wake_fd) {}

  ~InotifyWatcher() override { stop(); }

  void stop() override {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
      return;
    }
    const std::uint64_t one = 1;
    (void)::write(wake_fd_, &one, sizeof(one));
    if (thread_.joinable()) {
      thread_.join();
    }
    ::close(inotify_fd_);
    ::close(wake_fd_);
    inotify_fd_ = -1;
    wake_fd_ = -1;
  }

  void initialize() { add_watch_recursive(root_, ""); }

  void start() { thread_ = std::thread([this] { run(); }); }

 private:
  void run();
  void handle_event(const inotify_event& event);
  void add_watch_recursive(const std::filesystem::path& absolute, const std::string& relative);
  void remove_watches_under(const std::string& relative);
  [[nodiscard]] std::string directory_for(int wd) const;

  std::filesystem::path root_;
  WatchSink& sink_;
  int inotify_fd_;
  int wake_fd_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;

  mutable std::mutex mutex_;
  std::unordered_map<int, std::string> wd_to_directory_;
  std::unordered_map<std::uint32_t, PendingMove> pending_moves_;
};

std::string InotifyWatcher::directory_for(int wd) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = wd_to_directory_.find(wd);
  return it == wd_to_directory_.end() ? std::string{} : it->second;
}

void InotifyWatcher::remove_watches_under(const std::string& relative) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = wd_to_directory_.begin(); it != wd_to_directory_.end();) {
    const std::string& directory = it->second;
    const bool matches =
        directory == relative ||
        (directory.size() > relative.size() &&
         directory.compare(0, relative.size(), relative) == 0 &&
         directory[relative.size()] == '/');
    if (matches) {
      it = wd_to_directory_.erase(it);
    } else {
      ++it;
    }
  }
}

void InotifyWatcher::add_watch_recursive(const std::filesystem::path& absolute,
                                         const std::string& relative) {
  const int wd = ::inotify_add_watch(inotify_fd_, absolute.c_str(), kWatchMask);
  if (wd < 0) {
    sink_.on_error("inotify_add_watch failed: " + absolute.u8string());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wd_to_directory_[wd] = relative;
  }

  std::error_code error;
  std::filesystem::directory_iterator iterator(
      absolute, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    return;
  }
  for (const auto& entry : iterator) {
    std::error_code status_error;
    if (entry.is_symlink(status_error) || status_error) {
      continue;
    }
    if (!entry.is_directory(status_error) || status_error) {
      continue;
    }
    const std::string name = entry.path().filename().u8string();
    if (relative.empty() && name == ".syncbox") {
      continue;
    }
    const std::string child_relative =
        relative.empty() ? name : relative + "/" + name;
    add_watch_recursive(entry.path(), child_relative);
  }
}

void InotifyWatcher::handle_event(const inotify_event& event) {
  if ((event.mask & IN_Q_OVERFLOW) != 0) {
    sink_.on_error("inotify queue overflow");
    return;
  }
  if ((event.mask & IN_IGNORED) != 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    wd_to_directory_.erase(event.wd);
    return;
  }

  const std::string directory = directory_for(event.wd);
  std::string name;
  if (event.len > 0) {
    name.assign(event.name, ::strnlen(event.name, event.len));
  }

  std::string relative = directory;
  if (!name.empty()) {
    relative = directory.empty() ? name : directory + "/" + name;
  }
  if (relative.empty()) {
    return;
  }

  if ((event.mask & IN_MOVED_FROM) != 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_moves_[event.cookie] =
        PendingMove{relative, std::chrono::steady_clock::now()};
    return;
  }

  if ((event.mask & IN_MOVED_TO) != 0) {
    bool renamed = false;
    std::string old_path;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = pending_moves_.find(event.cookie);
      if (it != pending_moves_.end()) {
        old_path = it->second.path;
        pending_moves_.erase(it);
        renamed = true;
      }
    }
    if ((event.mask & IN_ISDIR) != 0) {
      add_watch_recursive(root_ / relative, relative);
    }
    if (renamed) {
      sink_.on_event(WatchEvent{WatchEventKind::Renamed, relative, old_path});
    } else {
      sink_.on_event(WatchEvent{WatchEventKind::Created, relative, std::nullopt});
    }
    return;
  }

  if ((event.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
    if ((event.mask & IN_ISDIR) != 0) {
      add_watch_recursive(root_ / relative, relative);
    }
    sink_.on_event(WatchEvent{WatchEventKind::Created, relative, std::nullopt});
    return;
  }

  if ((event.mask & IN_DELETE) != 0) {
    if ((event.mask & IN_ISDIR) != 0) {
      remove_watches_under(relative);
    }
    sink_.on_event(WatchEvent{WatchEventKind::Removed, relative, std::nullopt});
    return;
  }

  if ((event.mask & (IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF)) != 0) {
    sink_.on_event(WatchEvent{WatchEventKind::Modified, relative, std::nullopt});
  }
}

void InotifyWatcher::run() {
  std::vector<char> buffer(kEventBufferSize);

  while (!stopping_.load()) {
    pollfd descriptors[2] = {};
    descriptors[0].fd = inotify_fd_;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = wake_fd_;
    descriptors[1].events = POLLIN;

    const int ready = ::poll(descriptors, 2, 500);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      sink_.on_error(std::string("poll failed: ") + std::strerror(errno));
      return;
    }
    if (ready == 0) {
      continue;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      return;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
      continue;
    }

    while (true) {
      const ssize_t length = ::read(inotify_fd_, buffer.data(), buffer.size());
      if (length <= 0) {
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
          break;
        }
        if (length < 0) {
          sink_.on_error(std::string("inotify read failed: ") + std::strerror(errno));
        }
        return;
      }

      std::size_t offset = 0;
      while (offset < static_cast<std::size_t>(length)) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        handle_event(*event);
        offset += sizeof(inotify_event) + event->len;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pending_moves_.begin(); it != pending_moves_.end();) {
      if (now - it->second.seen > kMovePairingWindow) {
        it = pending_moves_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

}  // namespace

Result<std::unique_ptr<Watcher>> start_watcher(const std::filesystem::path& root,
                                               WatchSink& sink) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return make_error(ErrorCode::IoError, "watch root is not a directory");
  }

  const int inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd < 0) {
    return make_error(ErrorCode::IoError,
                      std::string("inotify_init1 failed: ") + std::strerror(errno));
  }
  const int wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    ::close(inotify_fd);
    return make_error(ErrorCode::IoError,
                      std::string("eventfd failed: ") + std::strerror(errno));
  }

  auto watcher = std::make_unique<InotifyWatcher>(root, sink, inotify_fd, wake_fd);
  watcher->initialize();
  watcher->start();
  return std::unique_ptr<Watcher>(std::move(watcher));
}

}  // namespace syncbox::fs
