#include <syncbox/fs/change_detector.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace syncbox::fs {

std::vector<LocalChange> diff_scans(const std::vector<ScannedEntry>& previous,
                                    const std::vector<ScannedEntry>& current) {
  std::map<std::string, const ScannedEntry*> previous_by_path;
  std::map<std::string, const ScannedEntry*> current_by_path;
  for (const auto& entry : previous) {
    previous_by_path[entry.path] = &entry;
  }
  for (const auto& entry : current) {
    current_by_path[entry.path] = &entry;
  }

  std::vector<LocalChange> changes;
  std::vector<const ScannedEntry*> removed;
  std::vector<const ScannedEntry*> added;

  for (const auto& [path, entry] : current_by_path) {
    const auto it = previous_by_path.find(path);
    if (it == previous_by_path.end()) {
      added.push_back(entry);
      continue;
    }
    const ScannedEntry& before = *it->second;
    if (before.size != entry->size || before.mtime_ms != entry->mtime_ms ||
        before.identity.inode != entry->identity.inode ||
        before.identity.device != entry->identity.device) {
      changes.push_back(LocalChange{ChangeKind::Modified, path, std::nullopt, entry->identity});
    }
  }

  for (const auto& [path, entry] : previous_by_path) {
    if (current_by_path.find(path) == current_by_path.end()) {
      removed.push_back(entry);
    }
  }

  // Pair renames by stable identity. Each identity may be used once.
  std::map<std::pair<std::uint64_t, std::uint64_t>, const ScannedEntry*> added_by_identity;
  for (const auto* entry : added) {
    if (entry->identity.valid()) {
      added_by_identity[{entry->identity.inode, entry->identity.device}] = entry;
    }
  }

  std::set<const ScannedEntry*> paired_added;
  for (const auto* entry : removed) {
    if (!entry->identity.valid()) {
      continue;
    }
    const auto it =
        added_by_identity.find({entry->identity.inode, entry->identity.device});
    if (it == added_by_identity.end() || paired_added.count(it->second) != 0) {
      continue;
    }
    const ScannedEntry* replacement = it->second;
    paired_added.insert(replacement);
    changes.push_back(LocalChange{ChangeKind::Renamed, replacement->path, entry->path,
                                  replacement->identity});
  }

  for (const auto* entry : added) {
    if (paired_added.count(entry) == 0) {
      changes.push_back(LocalChange{ChangeKind::Created, entry->path, std::nullopt,
                                    entry->identity});
    }
  }

  for (const auto* entry : removed) {
    bool renamed = false;
    for (const auto& change : changes) {
      if (change.kind == ChangeKind::Renamed && change.old_path.has_value() &&
          *change.old_path == entry->path) {
        renamed = true;
        break;
      }
    }
    if (!renamed) {
      changes.push_back(LocalChange{ChangeKind::Deleted, entry->path, std::nullopt,
                                    entry->identity});
    }
  }

  std::sort(changes.begin(), changes.end(), [](const LocalChange& lhs, const LocalChange& rhs) {
    return std::tie(lhs.path, lhs.kind) < std::tie(rhs.path, rhs.kind);
  });
  return changes;
}

}  // namespace syncbox::fs
