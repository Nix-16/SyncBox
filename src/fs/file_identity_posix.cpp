#include <syncbox/fs/types.hpp>

#include <sys/stat.h>

namespace syncbox::fs {

FileIdentity file_identity(const std::string& absolute_path) {
  struct stat info {};
  if (::stat(absolute_path.c_str(), &info) != 0) {
    return {};
  }
  FileIdentity identity;
  identity.inode = static_cast<std::uint64_t>(info.st_ino);
  identity.device = static_cast<std::uint64_t>(info.st_dev);
  return identity;
}

}  // namespace syncbox::fs
