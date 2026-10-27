#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/core/version_model.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace syncbox {

// 32 random bytes encoded as base64url without padding (43 characters).
// Used for enrollment and device tokens.
[[nodiscard]] Result<std::string> generate_token();

// SHA-256 of the token string; this is what gets stored on the server.
[[nodiscard]] Result<ContentHash> hash_token(std::string_view token);

// Constant-time comparison for equal-length byte sequences.
[[nodiscard]] bool constant_time_equal(const std::uint8_t* lhs, const std::uint8_t* rhs,
                                       std::size_t size) noexcept;

}  // namespace syncbox
