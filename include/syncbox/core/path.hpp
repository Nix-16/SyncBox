#pragma once

#include <syncbox/core/result.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace syncbox {

namespace path_rules {
inline constexpr std::size_t kMaxSegmentBytes = 255;
inline constexpr std::size_t kMaxPathBytes = 1024;
}  // namespace path_rules

// Validates and NFC-normalizes a relative path (DATABASE.md section 1.1).
// Returns PATH_INVALID on any rule violation.
[[nodiscard]] Result<std::string> normalize_relative_path(std::string_view input);

// path_key = casefold(NFC(path)); used for cross-platform uniqueness.
[[nodiscard]] Result<std::string> make_path_key(std::string_view input);

// Unicode helpers; both reject invalid UTF-8.
[[nodiscard]] Result<std::string> normalize_unicode_nfc(std::string_view input);
[[nodiscard]] Result<std::string> casefold_unicode(std::string_view input);

// Windows device names are reserved even with an extension (CON.txt).
[[nodiscard]] bool is_reserved_windows_name(std::string_view segment) noexcept;

}  // namespace syncbox
