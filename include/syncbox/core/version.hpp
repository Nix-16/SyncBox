#pragma once

#include <string_view>

namespace syncbox {

// Returns the project version string configured by CMake (e.g. "0.1.0").
[[nodiscard]] std::string_view version_string() noexcept;

// Returns the project name, always "SyncBox".
[[nodiscard]] std::string_view project_name() noexcept;

}  // namespace syncbox
