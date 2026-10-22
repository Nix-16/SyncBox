#include <gtest/gtest.h>

#include <syncbox/fs/debounce.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

TEST(DebouncerTest, CoalescesBurstIntoOneCallback) {
  std::mutex mutex;
  std::condition_variable condition;
  int callbacks = 0;

  syncbox::fs::Debouncer debouncer(std::chrono::milliseconds(30), [&] {
    std::lock_guard<std::mutex> lock(mutex);
    ++callbacks;
    condition.notify_all();
  });

  debouncer.notify();
  debouncer.notify();
  debouncer.notify();

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return callbacks > 0; }));
  EXPECT_EQ(callbacks, 1);

  lock.unlock();
  debouncer.stop();
}

TEST(DebouncerTest, StopWithoutNotifyIsSafe) {
  syncbox::fs::Debouncer debouncer(std::chrono::milliseconds(10), [] {});
  debouncer.stop();
  debouncer.stop();
}
