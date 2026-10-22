#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/fs/ignore_rules.hpp>
#include <syncbox/fs/types.hpp>

#include <filesystem>

namespace syncbox::fs {

// Recursively scans regular files under `root`. Directories are not emitted;
// symlinks and `<root>/.syncbox` are always skipped, and ignored directories
// are not traversed.
[[nodiscard]] Result<ScanResult> scan_directory(const std::filesystem::path& root,
                                                const IgnoreRules& ignore,
                                                const ScanOptions& options = {});

}  // namespace syncbox::fs
