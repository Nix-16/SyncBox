#pragma once

#include <syncbox/core/result.hpp>

#include <openssl/ssl.h>

#include <string_view>

namespace syncbox::net::detail {

// Verifies the peer certificate's SHA-256 fingerprint when `expected_hex` is
// non-empty. Accepts upper/lowercase hex with optional ':' separators.
[[nodiscard]] Result<void> verify_certificate_pin(SSL* ssl, std::string_view expected_hex);

}  // namespace syncbox::net::detail
