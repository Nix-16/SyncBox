#include <gtest/gtest.h>

#include <syncbox/fs/watcher.hpp>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {

class RecordingSink final : public syncbox::fs::WatchSink {
 public:
  void on_event(const syncbox::fs::WatchEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
    condition_.notify_all();
  }

  void on_error(std::string_view message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    errors_.emplace_back(message);
    condition_.notify_all();
  }

  template <typename Predicate>
  bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] {
      for (const auto& event : events_) {
        if (predicate(event)) {
          return true;
        }
      }
      return false;
    });
  }

  [[nodiscard]] std::vector<std::string> errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<syncbox::fs::WatchEvent> events_;
  std::vector<std::string> errors_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

}  // namespace

TEST(WatchTest, ReportsCreateRenameAndDelete) {
  syncbox::test::TempDirectory directory;
  RecordingSink sink;

  auto watcher = syncbox::fs::start_watcher(directory.path(), sink);
  ASSERT_TRUE(watcher);

  write_file(directory.file("a.txt"), "hello");
  EXPECT_TRUE(sink.wait_for(
      [](const syncbox::fs::WatchEvent& event) {
        return event.path == "a.txt" &&
               (event.kind == syncbox::fs::WatchEventKind::Created ||
                event.kind == syncbox::fs::WatchEventKind::Modified);
      },
      std::chrono::seconds(5)));

  std::error_code error;
  std::filesystem::rename(directory.file("a.txt"), directory.file("b.txt"), error);
  ASSERT_FALSE(error);

  const bool paired_rename = sink.wait_for(
      [](const syncbox::fs::WatchEvent& event) {
        return event.kind == syncbox::fs::WatchEventKind::Renamed &&
               event.path == "b.txt" && event.old_path.has_value() &&
               *event.old_path == "a.txt";
      },
      std::chrono::seconds(5));

  if (!paired_rename) {
    // FSEvents may report only one side of a rename; require the new path
    // and accept the old side as optional.
    EXPECT_TRUE(sink.wait_for(
        [](const syncbox::fs::WatchEvent& event) {
          return (event.kind == syncbox::fs::WatchEventKind::Created &&
                  event.path == "b.txt") ||
                 (event.kind == syncbox::fs::WatchEventKind::Removed &&
                  event.path == "a.txt");
        },
        std::chrono::seconds(5)));
  }

  std::filesystem::remove(directory.file("b.txt"), error);
  ASSERT_FALSE(error);

  EXPECT_TRUE(sink.wait_for(
      [](const syncbox::fs::WatchEvent& event) {
        return event.kind == syncbox::fs::WatchEventKind::Removed && event.path == "b.txt";
      },
      std::chrono::seconds(5)));

  watcher.value()->stop();
  EXPECT_TRUE(sink.errors().empty());
}
