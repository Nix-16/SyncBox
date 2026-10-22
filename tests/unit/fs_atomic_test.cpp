#include <gtest/gtest.h>

#include <syncbox/fs/atomic_file.hpp>

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

TEST(AtomicFileTest, CreatesTempFileInsideSyncboxDirectory) {
  syncbox::test::TempDirectory directory;
  auto temp = syncbox::fs::create_temp_file(directory.path());
  ASSERT_TRUE(temp);

  const auto relative =
      temp.value().lexically_relative(directory.path()).generic_u8string();
  EXPECT_EQ(relative.rfind(".syncbox/tmp/", 0), 0U);
  EXPECT_TRUE(std::filesystem::exists(temp.value()));

  std::filesystem::remove(temp.value());
}

TEST(AtomicFileTest, ReplacesTargetAndConsumesTemp) {
  syncbox::test::TempDirectory directory;
  const auto target = directory.file("target.txt");

  {
    std::ofstream stream(target, std::ios::binary);
    stream << "old";
  }

  auto temp = syncbox::fs::create_temp_file(directory.path());
  ASSERT_TRUE(temp);
  {
    std::ofstream stream(temp.value(), std::ios::binary | std::ios::trunc);
    stream << "new-content";
  }
  ASSERT_TRUE(syncbox::fs::sync_file(temp.value()));
  auto replaced = syncbox::fs::atomic_replace(target, temp.value());
  ASSERT_TRUE(replaced) << replaced.error().message;

  EXPECT_EQ(read_file(target), "new-content");
  EXPECT_FALSE(std::filesystem::exists(temp.value()));
}

TEST(AtomicFileTest, ReplacesMissingTarget) {
  syncbox::test::TempDirectory directory;
  const auto target = directory.file("new.txt");

  auto temp = syncbox::fs::create_temp_file(directory.path());
  ASSERT_TRUE(temp);
  {
    std::ofstream stream(temp.value(), std::ios::binary | std::ios::trunc);
    stream << "created";
  }
  ASSERT_TRUE(syncbox::fs::sync_file(temp.value()));
  auto replaced = syncbox::fs::atomic_replace(target, temp.value());
  ASSERT_TRUE(replaced) << replaced.error().message;

  EXPECT_EQ(read_file(target), "created");
}

TEST(AtomicFileTest, SyncsDirectory) {
  syncbox::test::TempDirectory directory;
  EXPECT_TRUE(syncbox::fs::sync_directory(directory.path()));
}
