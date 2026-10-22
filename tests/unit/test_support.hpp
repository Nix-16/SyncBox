#pragma once

#include <syncbox/core/uuid.hpp>
#include <syncbox/store/database.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace syncbox::test {

// Creates an isolated directory under the system temp dir and removes it
// (including WAL side files) when the test finishes.
class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("syncbox-test-" + generate_uuid_v7());
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] std::filesystem::path file(std::string_view name) const {
    return path_ / std::string(name);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

inline std::int64_t scalar_int(Database& db, std::string_view sql) {
  auto statement = Statement::prepare(db, sql);
  if (!statement) {
    return -1;
  }
  auto step = statement.value().step();
  if (!step || !step.value()) {
    return -1;
  }
  return statement.value().column_int64(0);
}

inline std::string scalar_text(Database& db, std::string_view sql) {
  auto statement = Statement::prepare(db, sql);
  if (!statement) {
    return {};
  }
  auto step = statement.value().step();
  if (!step || !step.value()) {
    return {};
  }
  return statement.value().column_text(0);
}

}  // namespace syncbox::test
