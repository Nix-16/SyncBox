#include <gtest/gtest.h>

#include <syncbox/core/path.hpp>

#include <string>

using syncbox::make_path_key;
using syncbox::normalize_relative_path;

TEST(PathTest, NormalizesValidRelativePaths) {
  const auto plain = normalize_relative_path("docs/spec.md");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.value(), "docs/spec.md");

  const auto nested = normalize_relative_path("a/b/c.txt");
  ASSERT_TRUE(nested);
  EXPECT_EQ(nested.value(), "a/b/c.txt");

  const auto unicode = normalize_relative_path(u8"报告/2026.txt");
  ASSERT_TRUE(unicode);
  EXPECT_EQ(unicode.value(), u8"报告/2026.txt");
}

TEST(PathTest, NormalizesToNfc) {
  // "cafe" + combining acute accent (U+0301) normalizes to U+00E9.
  const auto result = normalize_relative_path("cafe" "\xCC\x81" ".txt");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), "caf" "\xC3\xA9" ".txt");
}

TEST(PathTest, RejectsTraversalAndAbsolutePaths) {
  EXPECT_FALSE(normalize_relative_path(""));
  EXPECT_FALSE(normalize_relative_path("../etc/passwd"));
  EXPECT_FALSE(normalize_relative_path("a/../../b"));
  EXPECT_FALSE(normalize_relative_path("/absolute/path"));
  EXPECT_FALSE(normalize_relative_path("a//b"));
  EXPECT_FALSE(normalize_relative_path("a/./b"));
  EXPECT_FALSE(normalize_relative_path("a/"));
  EXPECT_FALSE(normalize_relative_path("a\\b"));
  EXPECT_FALSE(normalize_relative_path("..\\windows\\system32"));
  EXPECT_FALSE(normalize_relative_path(".hidden/.."));
}

TEST(PathTest, RejectsControlAndNulBytes) {
  EXPECT_FALSE(normalize_relative_path("a\tb"));
  EXPECT_FALSE(normalize_relative_path(std::string("a\0b", 3)));
  EXPECT_FALSE(normalize_relative_path("a\x7F" "b"));
}

TEST(PathTest, RejectsWindowsReservedNamesAndCharacters) {
  EXPECT_FALSE(normalize_relative_path("CON"));
  EXPECT_FALSE(normalize_relative_path("con.txt"));
  EXPECT_FALSE(normalize_relative_path("dir/NUL"));
  EXPECT_FALSE(normalize_relative_path("COM1.log"));
  EXPECT_FALSE(normalize_relative_path("a:b"));
  EXPECT_FALSE(normalize_relative_path("a*b"));
  EXPECT_FALSE(normalize_relative_path("a?b"));
  EXPECT_FALSE(normalize_relative_path("a|b"));
  EXPECT_FALSE(normalize_relative_path("a<b"));
  EXPECT_FALSE(normalize_relative_path("a>b"));
  EXPECT_FALSE(normalize_relative_path("a\"b"));
  EXPECT_FALSE(normalize_relative_path("file."));
  EXPECT_FALSE(normalize_relative_path("file "));
}

TEST(PathTest, EnforcesLengthLimits) {
  const std::string segment_255(255, 'a');
  const std::string segment_256(256, 'a');
  EXPECT_TRUE(normalize_relative_path(segment_255));
  EXPECT_FALSE(normalize_relative_path(segment_256));

  // 4 segments of 255 bytes + 3 separators = 1023 bytes: valid.
  const std::string path_1023 =
      segment_255 + "/" + segment_255 + "/" + segment_255 + "/" + segment_255;
  EXPECT_TRUE(normalize_relative_path(path_1023));

  // 5 segments exceed the 1024-byte limit.
  const std::string path_1279 = path_1023 + "/" + segment_255;
  EXPECT_FALSE(normalize_relative_path(path_1279));
}

TEST(PathTest, PathKeyFoldsCaseAndNormalization) {
  const auto upper = make_path_key("Report.TXT");
  const auto lower = make_path_key("report.txt");
  ASSERT_TRUE(upper);
  ASSERT_TRUE(lower);
  EXPECT_EQ(upper.value(), lower.value());

  const auto nfc = make_path_key("caf" "\xC3\xA9" ".txt");
  const auto nfd = make_path_key("cafe" "\xCC\x81" ".txt");
  ASSERT_TRUE(nfc);
  ASSERT_TRUE(nfd);
  EXPECT_EQ(nfc.value(), nfd.value());
}
