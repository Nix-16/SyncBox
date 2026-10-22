#include <syncbox/fs/types.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>

namespace syncbox::fs {

FileIdentity file_identity(const std::string& absolute_path) {
  const auto path = std::filesystem::u8path(absolute_path);
  const HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return {};
  }

  BY_HANDLE_FILE_INFORMATION info{};
  FileIdentity identity;
  if (GetFileInformationByHandle(handle, &info)) {
    identity.inode =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
    identity.device = static_cast<std::uint64_t>(info.dwVolumeSerialNumber);
  }
  CloseHandle(handle);
  return identity;
}

}  // namespace syncbox::fs
