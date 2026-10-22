#pragma once

#include <string>
#include <string_view>

namespace syncbox {

// Generates a UUIDv7 (time-ordered) in canonical lowercase form:
// xxxxxxxx-xxxx-7xxx-yxxx-xxxxxxxxxxxx
[[nodiscard]] std::string generate_uuid_v7();

// Returns true for a canonically formatted UUID (any version/variant).
[[nodiscard]] bool is_uuid(std::string_view text) noexcept;

}  // namespace syncbox
