#pragma once

#include <syncbox/core/result.hpp>

#include <filesystem>

namespace syncbox::fs {

// Creates <sync_root>/.syncbox/tmp/<uuid>.part and returns its path. The
// caller writes the payload, then calls atomic_replace().
[[nodiscard]] Result<std::filesystem::path> create_temp_file(
    const std::filesystem::path& sync_root);

// Flushes file data to stable storage.
[[nodiscard]] Result<void> sync_file(const std::filesystem::path& path);

// Flushes directory metadata (best effort on platforms without dir fsync).
[[nodiscard]] Result<void> sync_directory(const std::filesystem::path& path);

// Atomically replaces `target` with `temp`. Both must be on the same
// filesystem. `temp` is consumed on success.
[[nodiscard]] Result<void> atomic_replace(const std::filesystem::path& target,
                                          const std::filesystem::path& temp);

}  // namespace syncbox::fs
