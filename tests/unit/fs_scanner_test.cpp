#include <gtest/gtest.h>

#include <syncbox/fs/ignore_rules.hpp>
#include <syncbox/fs/scanner.hpp>
#include <syncbox/fs/types.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>

#include "test_support.hpp"

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream << content;
}

std::set<std::string> paths_of(const syncbox::fs::ScanResult& result) {
  std::set<std::string> paths;
  for (const auto& entry : result.entries) {
    paths.insert(entry.path);
  }
  return paths;
}

}  // namespace

TEST(ScannerTest, ScansRegularFilesRecursively) {
  syncbox::test::TempDirectory directory;

  write_file(directory.file("a.txt"), "aaa");
  write_file(directory.file("sub/b.txt"), "bb");
  write_file(directory.file("sub/deep/c.bin"), "c");

  const auto result = syncbox::fs::scan_directory(directory.path(), syncbox::fs::IgnoreRules{});
  ASSERT_TRUE(result);

  const auto paths = paths_of(result.value());
  EXPECT_EQ(paths, (std::set<std::string>{"a.txt", "sub/b.txt", "sub/deep/c.bin"}));
  EXPECT_TRUE(result.value().errors.empty());

  for (const auto& entry : result.value().entries) {
    EXPECT_GT(entry.mtime_ms, 0);
    EXPECT_TRUE(entry.identity.valid()) << entry.path;
    EXPECT_EQ(entry.kind, syncbox::fs::EntryKind::RegularFile);
  }
}

TEST(ScannerTest, SkipsInternalDirectoryAndIgnoredPatterns) {
  syncbox::test::TempDirectory directory;

  write_file(directory.file("keep.txt"), "keep");
  write_file(directory.file("skip.tmp"), "skip");
  write_file(directory.file("nested/skip.tmp"), "skip");
  write_file(directory.file("nested/keep.txt"), "keep");
  write_file(directory.file(".syncbox/tmp/partial.part"), "partial");
  write_file(directory.file("build/out.o"), "object");

  const syncbox::fs::IgnoreRules rules({"*.tmp", "build/**"});
  const auto result = syncbox::fs::scan_directory(directory.path(), rules);
  ASSERT_TRUE(result);

  const auto paths = paths_of(result.value());
  EXPECT_EQ(paths, (std::set<std::string>{"keep.txt", "nested/keep.txt"}));
  // skip.tmp, nested/skip.tmp and the build directory are ignored; .syncbox
  // is skipped as an internal directory and is not counted as user-ignored.
  EXPECT_EQ(result.value().ignored, 3U);
}

TEST(ScannerTest, RespectsMaxEntries) {
  syncbox::test::TempDirectory directory;
  for (int i = 0; i < 10; ++i) {
    write_file(directory.file("f" + std::to_string(i) + ".txt"), "x");
  }

  syncbox::fs::ScanOptions options;
  options.max_entries = 3;
  const auto result =
      syncbox::fs::scan_directory(directory.path(), syncbox::fs::IgnoreRules{}, options);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entries.size(), 3U);
}

TEST(ScannerTest, RejectsMissingRoot) {
  syncbox::test::TempDirectory directory;
  const auto result = syncbox::fs::scan_directory(
      directory.file("does-not-exist"), syncbox::fs::IgnoreRules{});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, syncbox::ErrorCode::IoError);
}
