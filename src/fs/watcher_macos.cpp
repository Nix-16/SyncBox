#include <syncbox/fs/watcher.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace syncbox::fs {

namespace {

[[nodiscard]] std::string cfstring_to_utf8(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(text);
  const CFIndex capacity =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<std::size_t>(capacity), '\0');
  if (!CFStringGetCString(text, result.data(), capacity, kCFStringEncodingUTF8)) {
    return {};
  }
  result.resize(std::char_traits<char>::length(result.c_str()));
  return result;
}

class MacWatcher final : public Watcher {
 public:
  MacWatcher(std::filesystem::path root, WatchSink& sink)
      : root_(std::move(root)), sink_(sink) {
    // FSEvents reports canonical paths; /var is a symlink to /private/var.
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root_, error);
    if (!error) {
      root_ = canonical;
    }
    root_prefix_ = root_.u8string();
    if (!root_prefix_.empty() && root_prefix_.back() != '/') {
      root_prefix_.push_back('/');
    }
  }

  ~MacWatcher() override { stop(); }

  Result<void> start() {
    CFStringRef root_string = CFStringCreateWithCString(
        kCFAllocatorDefault, root_.u8string().c_str(), kCFStringEncodingUTF8);
    if (root_string == nullptr) {
      return make_error(ErrorCode::IoError, "cannot create watch path string");
    }
    CFArrayRef paths =
        CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void**>(&root_string), 1,
                      &kCFTypeArrayCallBacks);
    CFRelease(root_string);
    if (paths == nullptr) {
      return make_error(ErrorCode::IoError, "cannot create watch path array");
    }

    FSEventStreamContext context{};
    context.info = this;

    stream_ = FSEventStreamCreate(
        kCFAllocatorDefault, &MacWatcher::callback, &context, paths,
        kFSEventStreamEventIdSinceNow, 0.1,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseCFTypes |
            kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (stream_ == nullptr) {
      return make_error(ErrorCode::IoError, "FSEventStreamCreate failed");
    }

    queue_ = dispatch_queue_create("syncbox.fsevents", DISPATCH_QUEUE_SERIAL);
    if (queue_ == nullptr) {
      FSEventStreamRelease(stream_);
      stream_ = nullptr;
      return make_error(ErrorCode::IoError, "cannot create dispatch queue");
    }

    FSEventStreamSetDispatchQueue(stream_, queue_);
    if (!FSEventStreamStart(stream_)) {
      FSEventStreamInvalidate(stream_);
      FSEventStreamRelease(stream_);
      stream_ = nullptr;
      dispatch_release(queue_);
      queue_ = nullptr;
      return make_error(ErrorCode::IoError, "FSEventStreamStart failed");
    }
    return Result<void>{};
  }

  void stop() override {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
      return;
    }
    if (stream_ != nullptr) {
      FSEventStreamStop(stream_);
      FSEventStreamInvalidate(stream_);
      FSEventStreamRelease(stream_);
      stream_ = nullptr;
    }
    if (queue_ != nullptr) {
      dispatch_release(queue_);
      queue_ = nullptr;
    }
  }

 private:
  static void callback(ConstFSEventStreamRef, void* info, std::size_t event_count,
                       void* event_paths, const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId[]) {
    auto* self = static_cast<MacWatcher*>(info);
    auto* paths = static_cast<CFArrayRef>(event_paths);

    for (std::size_t i = 0; i < event_count; ++i) {
      const auto* path =
          static_cast<CFStringRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
      self->handle_path(cfstring_to_utf8(path), flags[i]);
    }
  }

  void handle_path(const std::string& absolute, FSEventStreamEventFlags flags) {
    std::string relative;
    if (absolute == root_prefix_) {
      return;
    }
    if (absolute.compare(0, root_prefix_.size(), root_prefix_) == 0) {
      relative = absolute.substr(root_prefix_.size());
    } else {
      return;
    }
    if (relative.empty() || relative == ".syncbox" ||
        relative.compare(0, 9, ".syncbox/") == 0) {
      return;
    }

    if ((flags & kFSEventStreamEventFlagItemRemoved) != 0) {
      sink_.on_event(WatchEvent{WatchEventKind::Removed, relative, std::nullopt});
      return;
    }
    if ((flags & kFSEventStreamEventFlagItemCreated) != 0) {
      sink_.on_event(WatchEvent{WatchEventKind::Created, relative, std::nullopt});
      return;
    }
    if ((flags & kFSEventStreamEventFlagItemRenamed) != 0) {
      // FSEvents reports both sides of a rename without pairing; the scanner
      // re-pairs them by file identity. Treat existing paths as created.
      std::error_code error;
      if (std::filesystem::exists(root_ / relative, error) && !error) {
        sink_.on_event(WatchEvent{WatchEventKind::Created, relative, std::nullopt});
      } else {
        sink_.on_event(WatchEvent{WatchEventKind::Removed, relative, std::nullopt});
      }
      return;
    }
    sink_.on_event(WatchEvent{WatchEventKind::Modified, relative, std::nullopt});
  }

  std::filesystem::path root_;
  std::string root_prefix_;
  WatchSink& sink_;
  FSEventStreamRef stream_ = nullptr;
  dispatch_queue_t queue_ = nullptr;
  std::atomic<bool> stopping_{false};
};

}  // namespace

Result<std::unique_ptr<Watcher>> start_watcher(const std::filesystem::path& root,
                                               WatchSink& sink) {
  auto watcher = std::make_unique<MacWatcher>(root, sink);
  auto started = watcher->start();
  if (!started) {
    return started.error();
  }
  return std::unique_ptr<Watcher>(std::move(watcher));
}

}  // namespace syncbox::fs
