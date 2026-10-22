#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace syncbox::fs {

enum class EntryKind {
  RegularFile,
  Directory,
  Symlink,
  Other,
};

// Stable identity used for rename detection (inode/device on POSIX,
// file index/volume serial on Windows). Zero means "not available".
struct FileIdentity {
  std::uint64_t inode = 0;
  std::uint64_t device = 0;

  [[nodiscard]] bool valid() const noexcept { return inode != 0; }
};

struct ScannedEntry {
  std::string path;  // normalized relative path, '/' separators, UTF-8
  EntryKind kind = EntryKind::RegularFile;
  std::uint64_t size = 0;
  std::int64_t mtime_ms = 0;
  FileIdentity identity;
};

struct ScanResult {
  std::vector<ScannedEntry> entries;
  std::vector<std::string> errors;  // relative paths that could not be read
  std::size_t ignored = 0;
};

struct ScanOptions {
  bool follow_symlinks = false;
  std::size_t max_entries = 0;  // 0 means unlimited
};

struct PathSupport {
  bool case_sensitive = true;
  bool normalizes_unicode = false;
  std::size_t max_path_bytes = 4096;
  std::size_t max_segment_bytes = 255;

  [[nodiscard]] static PathSupport platform();
};

[[nodiscard]] FileIdentity file_identity(const std::string& absolute_path);

}  // namespace syncbox::fs
