#include <syncbox/fs/atomic_file.hpp>

#include <syncbox/core/uuid.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace syncbox::fs {

namespace {

[[nodiscard]] Error errno_error(std::string_view action) {
  return make_error(ErrorCode::IoError,
                    std::string(action) + ": " + std::strerror(errno));
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
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    return errno_error("cannot create temp file");
  }
  ::close(fd);
  return path;
}

Result<void> sync_file(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return errno_error("cannot open file for fsync");
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  if (result != 0) {
    errno = saved_errno;
    return errno_error("fsync failed");
  }
  return Result<void>{};
}

Result<void> sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return errno_error("cannot open directory for fsync");
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  if (result != 0) {
    errno = saved_errno;
    return errno_error("directory fsync failed");
  }
  return Result<void>{};
}

Result<void> atomic_replace(const std::filesystem::path& target,
                            const std::filesystem::path& temp) {
  if (::rename(temp.c_str(), target.c_str()) != 0) {
    return errno_error("rename failed");
  }

  const auto parent = target.parent_path();
  if (!parent.empty()) {
    return sync_directory(parent);
  }
  return Result<void>{};
}

}  // namespace syncbox::fs
