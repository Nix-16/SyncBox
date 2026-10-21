#include <gtest/gtest.h>

#include <syncbox/core/error.hpp>
#include <syncbox/core/result.hpp>
#include <syncbox/core/version.hpp>

TEST(CoreTest, VersionIsNotEmpty) {
  EXPECT_FALSE(syncbox::version_string().empty());
  EXPECT_EQ(syncbox::project_name(), "SyncBox");
}

TEST(ErrorTest, WireCodeMapping) {
  using syncbox::ErrorCode;

  EXPECT_EQ(syncbox::to_string(ErrorCode::RevisionConflict), "REVISION_CONFLICT");
  EXPECT_EQ(syncbox::to_string(ErrorCode::PathInvalid), "PATH_INVALID");
  EXPECT_EQ(syncbox::http_status(ErrorCode::RevisionConflict), 409);
  EXPECT_EQ(syncbox::http_status(ErrorCode::CursorExpired), 410);
  EXPECT_EQ(syncbox::http_status(ErrorCode::IoError), 0);
  EXPECT_TRUE(syncbox::is_retryable(ErrorCode::Unavailable));
  EXPECT_FALSE(syncbox::is_retryable(ErrorCode::PathInvalid));
}

TEST(ErrorTest, MakeErrorDerivesRetryable) {
  const auto error = syncbox::make_error(syncbox::ErrorCode::RateLimited, "slow down");

  EXPECT_EQ(error.code, syncbox::ErrorCode::RateLimited);
  EXPECT_EQ(error.message, "slow down");
  EXPECT_TRUE(error.retryable);
  EXPECT_EQ(syncbox::to_string(error.code), "RATE_LIMITED");
}

TEST(ResultTest, HoldsValueOrError) {
  syncbox::Result<int> ok = 42;
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok.value(), 42);

  syncbox::Result<int> bad = syncbox::make_error(syncbox::ErrorCode::IoError);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, syncbox::ErrorCode::IoError);
  EXPECT_TRUE(bad.error().retryable);
}
