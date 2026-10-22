#include <gtest/gtest.h>

#include <syncbox/fs/suppression.hpp>

#include <chrono>

TEST(WriteSuppressionTest, SuppressesWithinWindow) {
  syncbox::fs::WriteSuppression suppression;

  EXPECT_FALSE(suppression.is_suppressed("a.txt"));
  suppression.suppress("a.txt", std::chrono::hours(1));
  EXPECT_TRUE(suppression.is_suppressed("a.txt"));
  EXPECT_FALSE(suppression.is_suppressed("b.txt"));
}

TEST(WriteSuppressionTest, ExpiredEntriesAreNotSuppressed) {
  syncbox::fs::WriteSuppression suppression;
  suppression.suppress("a.txt", std::chrono::milliseconds(0));
  EXPECT_FALSE(suppression.is_suppressed("a.txt"));
}

TEST(WriteSuppressionTest, ClearRemovesAllEntries) {
  syncbox::fs::WriteSuppression suppression;
  suppression.suppress("a.txt", std::chrono::hours(1));
  suppression.suppress("b.txt", std::chrono::hours(1));

  suppression.clear();
  EXPECT_FALSE(suppression.is_suppressed("a.txt"));
  EXPECT_FALSE(suppression.is_suppressed("b.txt"));
}
