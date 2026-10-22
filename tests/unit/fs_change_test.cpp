#include <gtest/gtest.h>

#include <syncbox/fs/change_detector.hpp>
#include <syncbox/fs/ignore_rules.hpp>
#include <syncbox/fs/scanner.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

std::vector<syncbox::fs::ScannedEntry> scan(const std::filesystem::path& root) {
  auto result = syncbox::fs::scan_directory(root, syncbox::fs::IgnoreRules{});
  EXPECT_TRUE(result);
  return result ? result.value().entries : std::vector<syncbox::fs::ScannedEntry>{};
}

const syncbox::fs::LocalChange* find_change(const std::vector<syncbox::fs::LocalChange>& changes,
                                            syncbox::fs::ChangeKind kind,
                                            const std::string& path) {
  for (const auto& change : changes) {
    if (change.kind == kind && change.path == path) {
      return &change;
    }
  }
  return nullptr;
}

}  // namespace

TEST(ChangeDetectorTest, DetectsCreatedModifiedAndDeleted) {
  syncbox::test::TempDirectory directory;
  write_file(directory.file("a.txt"), "aaa");

  const auto before = scan(directory.path());

  write_file(directory.file("a.txt"), "aaaa");
  write_file(directory.file("b.txt"), "bbb");
  const auto after = scan(directory.path());

  const auto changes = syncbox::fs::diff_scans(before, after);
  ASSERT_NE(find_change(changes, syncbox::fs::ChangeKind::Modified, "a.txt"), nullptr);
  ASSERT_NE(find_change(changes, syncbox::fs::ChangeKind::Created, "b.txt"), nullptr);
  EXPECT_EQ(find_change(changes, syncbox::fs::ChangeKind::Deleted, "a.txt"), nullptr);

  std::filesystem::remove(directory.file("b.txt"));
  const auto after_delete = scan(directory.path());
  const auto deletion_changes = syncbox::fs::diff_scans(after, after_delete);
  ASSERT_NE(find_change(deletion_changes, syncbox::fs::ChangeKind::Deleted, "b.txt"), nullptr);
}

TEST(ChangeDetectorTest, PairsRenameByFileIdentity) {
  syncbox::test::TempDirectory directory;
  write_file(directory.file("a.txt"), "content");

  const auto before = scan(directory.path());

  std::error_code error;
  std::filesystem::create_directories(directory.file("sub"));
  std::filesystem::rename(directory.file("a.txt"), directory.file("sub/b.txt"), error);
  ASSERT_FALSE(error);

  const auto after = scan(directory.path());
  const auto changes = syncbox::fs::diff_scans(before, after);

  const auto* renamed = find_change(changes, syncbox::fs::ChangeKind::Renamed, "sub/b.txt");
  ASSERT_NE(renamed, nullptr);
  ASSERT_TRUE(renamed->old_path.has_value());
  EXPECT_EQ(*renamed->old_path, "a.txt");
  EXPECT_EQ(find_change(changes, syncbox::fs::ChangeKind::Created, "sub/b.txt"), nullptr);
  EXPECT_EQ(find_change(changes, syncbox::fs::ChangeKind::Deleted, "a.txt"), nullptr);
}

TEST(ChangeDetectorTest, ReconcileDetectsChangesMissedByWatcher) {
  // FI-13 at the filesystem level: no watcher is running, so the changes are
  // invisible until a periodic scan compares snapshots.
  syncbox::test::TempDirectory directory;
  write_file(directory.file("kept.txt"), "one");
  const auto before = scan(directory.path());

  write_file(directory.file("kept.txt"), "two-longer");
  write_file(directory.file("missed.txt"), "created while unwatched");
  std::filesystem::remove(directory.file("kept.txt"));
  write_file(directory.file("kept.txt"), "recreated");

  const auto after = scan(directory.path());
  const auto changes = syncbox::fs::diff_scans(before, after);

  ASSERT_NE(find_change(changes, syncbox::fs::ChangeKind::Created, "missed.txt"), nullptr);
  ASSERT_NE(find_change(changes, syncbox::fs::ChangeKind::Modified, "kept.txt"), nullptr);
}

TEST(ChangeDetectorTest, IdenticalScansProduceNoChanges) {
  syncbox::test::TempDirectory directory;
  write_file(directory.file("a.txt"), "aaa");

  const auto before = scan(directory.path());
  const auto after = scan(directory.path());
  EXPECT_TRUE(syncbox::fs::diff_scans(before, after).empty());
}
