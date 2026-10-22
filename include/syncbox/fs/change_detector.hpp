#pragma once

#include <syncbox/fs/types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace syncbox::fs {

enum class ChangeKind {
  Created,
  Modified,
  Deleted,
  Renamed,
};

struct LocalChange {
  ChangeKind kind = ChangeKind::Modified;
  std::string path;                     // new path (or the only path)
  std::optional<std::string> old_path;  // set for Renamed
  FileIdentity identity;
};

// Compares two scans of the same root. Renames are paired by stable file
// identity (inode/file index); filesystems that do not expose one degrade to
// Deleted + Created, which is correct but loses history continuity.
[[nodiscard]] std::vector<LocalChange> diff_scans(
    const std::vector<ScannedEntry>& previous,
    const std::vector<ScannedEntry>& current);

}  // namespace syncbox::fs
