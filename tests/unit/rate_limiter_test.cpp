#include <gtest/gtest.h>

#include <syncbox/server/rate_limiter.hpp>

TEST(RateLimiterTest, AllowsRequestsUpToTheLimit) {
  syncbox::server::RateLimiter limiter;

  EXPECT_TRUE(limiter.allow("device-a:changes", 3));
  EXPECT_TRUE(limiter.allow("device-a:changes", 3));
  EXPECT_TRUE(limiter.allow("device-a:changes", 3));
  EXPECT_FALSE(limiter.allow("device-a:changes", 3));
}

TEST(RateLimiterTest, KeysAreIndependent) {
  syncbox::server::RateLimiter limiter;

  EXPECT_TRUE(limiter.allow("device-a:ops", 1));
  EXPECT_FALSE(limiter.allow("device-a:ops", 1));
  EXPECT_TRUE(limiter.allow("device-b:ops", 1));
  EXPECT_TRUE(limiter.allow("device-a:changes", 1));
}

TEST(RateLimiterTest, ZeroLimitDisablesLimiting) {
  syncbox::server::RateLimiter limiter;
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(limiter.allow("key", 0));
  }
}

TEST(RateLimiterTest, ResetClearsBuckets) {
  syncbox::server::RateLimiter limiter;
  EXPECT_TRUE(limiter.allow("key", 1));
  EXPECT_FALSE(limiter.allow("key", 1));

  limiter.reset();
  EXPECT_TRUE(limiter.allow("key", 1));
}
