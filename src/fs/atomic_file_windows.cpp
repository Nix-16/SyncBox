#include <syncbox/fs/atomic_file.hpp>

#include <syncbox/core/uuid.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace syncbox::fs {

namespace {

[[nodiscard]] Error win_error(std::string_view action) {
  return make_error(ErrorCode::IoError,
                    std::string(action) + " (Windows error " +
                        std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
}

[[nodiscard]] Error win_error_code(std::string_view action, DWORD code) {
  return make_error(ErrorCode::IoError,
                    std::string(action) + " (Windows error " +
                        std::to_string(static_cast<unsigned long>(code)) + ")");
}

}  // namespace

Result<std::filesystem::path> create_temp_file(const std::filesystem::path& sync_root) {
  const auto temp_directory = sync_root / ".syncbox" / "tmp";

  std::error_code error;
  std::filesystem::create_directories(temp_directory, error);
  if (error) {
    return make_error(ErrorCode::IoError,
                      "cannot create temp directory: " + error.message());
  }

  const auto path = temp_directory / (generate_uuid_v7() + ".part");
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win_error("cannot create temp file");
  }
  CloseHandle(handle);
  return path;
}

Result<void> sync_file(const std::filesystem::path& path) {
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win_error("cannot open file for FlushFileBuffers");
  }
  const BOOL result = FlushFileBuffers(handle);
  CloseHandle(handle);
  if (!result) {
    return win_error("FlushFileBuffers failed");
  }
  return Result<void>{};
}

Result<void> sync_directory(const std::filesystem::path& path) {
  const HANDLE handle = CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win_error("cannot open directory for FlushFileBuffers");
  }
  const BOOL result = FlushFileBuffers(handle);
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!result) {
    // Directory flush is a durability hint; several filesystems (and some
    // CI images) reject it with ERROR_INVALID_FUNCTION or ACCESS_DENIED.
    if (error == ERROR_INVALID_FUNCTION || error == ERROR_ACCESS_DENIED ||
        error == ERROR_INVALID_HANDLE) {
      return Result<void>{};
    }
    return win_error_code("directory FlushFileBuffers failed", error);
  }
  return Result<void>{};
}

Result<void> atomic_replace(const std::filesystem::path& target,
                            const std::filesystem::path& temp) {
  if (!ReplaceFileW(target.c_str(), temp.c_str(), nullptr,
                    REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    // ReplaceFileW only works when the destination exists and is not open.
    // MOVEFILE_REPLACE_EXISTING is atomic for paths on the same volume and
    // also covers the "target does not exist yet" case.
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return win_error("ReplaceFileW/MoveFileExW failed");
    }
  }

  // The rename is the atomic operation. Directory FlushFileBuffers is a
  // durability hint and is not supported on every Windows filesystem, so a
  // failure here must not turn a successful replace into an error.
  const auto parent = target.parent_path();
  if (!parent.empty()) {
    (void)sync_directory(parent);
  }
  return Result<void>{};
}

}  // namespace syncbox::fs
