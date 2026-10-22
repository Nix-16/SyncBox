#include <syncbox/fs/watcher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace syncbox::fs {

namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_CREATION;

[[nodiscard]] std::string to_utf8(const wchar_t* text, int length) {
  if (length <= 0) {
    return {};
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), required, nullptr, nullptr);
  return result;
}

[[nodiscard]] std::string normalize_relative(std::string path) {
  for (char& c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  return path;
}

class WindowsWatcher final : public Watcher {
 public:
  WindowsWatcher(HANDLE directory, WatchSink& sink, HANDLE event)
      : directory_(directory), sink_(sink) {
    overlapped_.hEvent = event;
  }

  ~WindowsWatcher() override { stop(); }

  void start() {
    thread_ = std::thread([this] { run(); });
    // Do not return until ReadDirectoryChangesW is armed, otherwise a write
    // racing with start_watcher() can be missed.
    std::unique_lock<std::mutex> lock(armed_mutex_);
    armed_condition_.wait_for(lock, std::chrono::seconds(1),
                              [this] { return armed_ || stopping_.load(); });
  }

  void stop() override {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
      return;
    }
    if (overlapped_.hEvent != nullptr) {
      ::SetEvent(overlapped_.hEvent);
    }
    ::CancelIoEx(directory_, &overlapped_);
    if (thread_.joinable()) {
      thread_.join();
    }
    if (overlapped_.hEvent != nullptr) {
      ::CloseHandle(overlapped_.hEvent);
      overlapped_.hEvent = nullptr;
    }
    ::CloseHandle(directory_);
    directory_ = INVALID_HANDLE_VALUE;
  }

 private:
  void run() {
    std::vector<unsigned char> buffer(kBufferSize);

    while (!stopping_.load()) {
      ::ResetEvent(overlapped_.hEvent);
      const BOOL issued = ::ReadDirectoryChangesW(
          directory_, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE, kFilter,
          nullptr, &overlapped_, nullptr);
      if (!issued) {
        if (!stopping_.load()) {
          sink_.on_error("ReadDirectoryChangesW failed to start");
        }
        return;
      }
      {
        std::lock_guard<std::mutex> lock(armed_mutex_);
        armed_ = true;
      }
      armed_condition_.notify_all();

      while (!stopping_.load()) {
        const DWORD wait = ::WaitForSingleObject(overlapped_.hEvent, 200);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          sink_.on_error("waiting for directory changes failed");
          return;
        }
      }
      if (stopping_.load()) {
        return;
      }

      DWORD bytes = 0;
      if (!::GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE)) {
        if (!stopping_.load()) {
          sink_.on_error("GetOverlappedResult failed");
        }
        return;
      }
      if (bytes == 0) {
        sink_.on_error("ReadDirectoryChangesW buffer overflow");
        continue;
      }

      std::optional<std::string> renamed_from;
      std::size_t offset = 0;
      while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes) {
        const auto* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
        const std::string name =
            normalize_relative(to_utf8(info->FileName,
                                       static_cast<int>(info->FileNameLength / sizeof(wchar_t))));

        switch (info->Action) {
          case FILE_ACTION_ADDED:
            sink_.on_event(WatchEvent{WatchEventKind::Created, name, std::nullopt});
            break;
          case FILE_ACTION_REMOVED:
            sink_.on_event(WatchEvent{WatchEventKind::Removed, name, std::nullopt});
            break;
          case FILE_ACTION_MODIFIED:
            sink_.on_event(WatchEvent{WatchEventKind::Modified, name, std::nullopt});
            break;
          case FILE_ACTION_RENAMED_OLD_NAME:
            renamed_from = name;
            break;
          case FILE_ACTION_RENAMED_NEW_NAME:
            sink_.on_event(WatchEvent{WatchEventKind::Renamed, name, renamed_from});
            renamed_from.reset();
            break;
          default:
            break;
        }

        if (info->NextEntryOffset == 0) {
          break;
        }
        offset += info->NextEntryOffset;
      }
    }
  }

  HANDLE directory_;
  WatchSink& sink_;
  OVERLAPPED overlapped_{};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex armed_mutex_;
  std::condition_variable armed_condition_;
  bool armed_ = false;
};

}  // namespace

Result<std::unique_ptr<Watcher>> start_watcher(const std::filesystem::path& root,
                                               WatchSink& sink) {
  const HANDLE directory = ::CreateFileW(
      root.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    return make_error(ErrorCode::IoError, "cannot open directory for watching");
  }

  const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    ::CloseHandle(directory);
    return make_error(ErrorCode::IoError, "cannot create watch event");
  }

  auto watcher = std::make_unique<WindowsWatcher>(directory, sink, event);
  watcher->start();
  return std::unique_ptr<Watcher>(std::move(watcher));
}

}  // namespace syncbox::fs
