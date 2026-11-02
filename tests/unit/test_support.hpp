#pragma once

#include <syncbox/core/uuid.hpp>
#include <syncbox/store/database.hpp>

#include <cstdint>
#include <cstdlib>
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

// Sets an environment variable for the lifetime of the object and restores
// the previous value afterwards.
class EnvironmentVariable {
 public:
  EnvironmentVariable(const char* name, const char* value) : name_(name) {
    const char* existing = std::getenv(name);
    if (existing != nullptr) {
      previous_ = existing;
      had_previous_ = true;
    }
    set(value);
  }

  ~EnvironmentVariable() {
    if (had_previous_) {
      set(previous_.c_str());
    } else {
      unset();
    }
  }

  EnvironmentVariable(const EnvironmentVariable&) = delete;
  EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;

 private:
  void set(const char* value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value);
#else
    setenv(name_.c_str(), value, 1);
#endif
  }

  void unset() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), "");
#else
    unsetenv(name_.c_str());
#endif
  }

  std::string name_;
  std::string previous_;
  bool had_previous_ = false;
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
