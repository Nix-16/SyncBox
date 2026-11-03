#include <gtest/gtest.h>

#include <syncbox/common/audit.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include "test_support.hpp"

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(AuditTest, WritesJsonLinesAndNeverLogsRawTokens) {
  syncbox::test::TempDirectory directory;
  const auto path = directory.file("audit.log");

  syncbox::audit::configure(path.string());
  syncbox::audit::auth_failure_for_token("super-secret-token-value", "invalid token");
  syncbox::audit::device_revoked("device-1", "admin", "test");
  syncbox::audit::flush();
  syncbox::audit::reset();

  const std::string content = read_file(path);
  EXPECT_NE(content.find("\"event\":\"auth_failure\""), std::string::npos);
  EXPECT_NE(content.find("\"token_hash_prefix\":"), std::string::npos);
  EXPECT_EQ(content.find("super-secret-token-value"), std::string::npos);
  EXPECT_NE(content.find("\"event\":\"device_revoked\""), std::string::npos);
  EXPECT_NE(content.find("\"device_id\":\"device-1\""), std::string::npos);
}

TEST(AuditTest, EscapesJsonValues) {
  syncbox::test::TempDirectory directory;
  const auto path = directory.file("audit.log");

  syncbox::audit::configure(path.string());
  syncbox::audit::event("test_event", {{"reason", "quote\" and \\slash"}});
  syncbox::audit::flush();
  syncbox::audit::reset();

  const std::string content = read_file(path);
  EXPECT_NE(content.find("quote\\\" and \\\\slash"), std::string::npos);
}
