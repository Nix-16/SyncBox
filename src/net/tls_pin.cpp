#include "tls_pin.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <array>
#include <cctype>

namespace syncbox::net::detail {

namespace {

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Result<void> verify_certificate_pin(SSL* ssl, std::string_view expected_hex) {
  if (expected_hex.empty()) {
    return Result<void>{};
  }

  std::string normalized;
  normalized.reserve(64);
  for (const char c : expected_hex) {
    if (c == ':' || std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    normalized.push_back(c);
  }
  if (normalized.size() != 64) {
    return make_error(ErrorCode::Internal, "certificate pin must be 64 hex characters");
  }

  std::array<unsigned char, 32> expected{};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const int high = hex_value(normalized[i * 2]);
    const int low = hex_value(normalized[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::Internal, "certificate pin contains non-hex characters");
    }
    expected[i] = static_cast<unsigned char>((high << 4) | low);
  }

  X509* certificate = SSL_get_peer_certificate(ssl);
  if (certificate == nullptr) {
    return make_error(ErrorCode::Unavailable, "TLS peer did not present a certificate");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  const int digest_ok = X509_digest(certificate, EVP_sha256(), digest.data(), &length);
  X509_free(certificate);
  if (digest_ok != 1 || length != expected.size()) {
    return make_error(ErrorCode::Internal, "cannot compute certificate fingerprint");
  }

  if (CRYPTO_memcmp(digest.data(), expected.data(), expected.size()) != 0) {
    return make_error(ErrorCode::Unavailable, "certificate fingerprint does not match pin");
  }
  return Result<void>{};
}

}  // namespace syncbox::net::detail
