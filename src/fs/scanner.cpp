#include <syncbox/fs/scanner.hpp>

#include <chrono>
#include <string>

namespace syncbox::fs {

namespace {

constexpr std::string_view kInternalDirectory = ".syncbox";

[[nodiscard]] bool is_internal_path(std::string_view relative) noexcept {
  return relative == kInternalDirectory ||
         (relative.size() > kInternalDirectory.size() &&
          relative.compare(0, kInternalDirectory.size(), kInternalDirectory) == 0 &&
          relative[kInternalDirectory.size()] == '/');
}

[[nodiscard]] std::int64_t to_unix_millis(std::filesystem::file_time_type file_time) {
  using namespace std::chrono;
  const auto system_now = system_clock::now();
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto system_time = system_now + (file_time - file_now);
  return duration_cast<milliseconds>(system_time.time_since_epoch()).count();
}

}  // namespace

Result<ScanResult> scan_directory(const std::filesystem::path& root,
                                  const IgnoreRules& ignore, const ScanOptions& options) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return make_error(ErrorCode::IoError, "sync root is not a directory: " + root.u8string());
  }

  ScanResult result;

  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot open sync root: " + error.message());
  }

  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry& entry = *iterator;

    std::error_code status_error;
    const auto status = entry.symlink_status(status_error);
    if (status_error) {
      result.errors.push_back(entry.path().lexically_relative(root).generic_u8string());
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    const std::string relative =
        entry.path().lexically_relative(root).generic_u8string();

    if (is_internal_path(relative)) {
      iterator.disable_recursion_pending();
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    if (std::filesystem::is_symlink(status)) {
      // Symlinks are out of scope for v1 (PRD section 7).
      ++result.ignored;
      if (options.follow_symlinks && std::filesystem::is_directory(entry.status(status_error))) {
        // Explicitly opted in; traversal continues.
      } else {
        iterator.disable_recursion_pending();
      }
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    if (std::filesystem::is_directory(status)) {
      if (ignore.ignored(relative)) {
        ++result.ignored;
        iterator.disable_recursion_pending();
      }
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    if (!std::filesystem::is_regular_file(status)) {
      ++result.ignored;
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    if (ignore.ignored(relative)) {
      ++result.ignored;
      iterator.increment(error);
      if (error) {
        result.errors.push_back(error.message());
        error.clear();
      }
      continue;
    }

    ScannedEntry scanned;
    scanned.path = relative;
    scanned.kind = EntryKind::RegularFile;
    scanned.mtime_ms = to_unix_millis(entry.last_write_time(status_error));
    if (status_error) {
      scanned.mtime_ms = 0;
      status_error.clear();
    }
    scanned.size = entry.file_size(status_error);
    if (status_error) {
      scanned.size = 0;
      status_error.clear();
    }
    scanned.identity = file_identity(entry.path().u8string());
    result.entries.push_back(std::move(scanned));

    if (options.max_entries != 0 && result.entries.size() >= options.max_entries) {
      break;
    }

    iterator.increment(error);
    if (error) {
      result.errors.push_back(error.message());
      error.clear();
    }
  }

  return result;
}

}  // namespace syncbox::fs
