#include <syncbox/core/token.hpp>

#include <syncbox/core/hash.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdlib>

namespace syncbox {

Result<std::string> generate_token() {
  constexpr std::size_t kTokenBytes = 32;
  std::array<unsigned char, kTokenBytes> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return make_error(ErrorCode::Internal, "RAND_bytes failed");
  }

  std::string encoded(static_cast<std::size_t>(EVP_ENCODE_LENGTH(kTokenBytes)), '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                      bytes.data(), static_cast<int>(bytes.size()));
  if (written <= 0) {
    return make_error(ErrorCode::Internal, "base64 encoding failed");
  }
  encoded.resize(static_cast<std::size_t>(written));

  std::string url_safe;
  url_safe.reserve(encoded.size());
  for (const char c : encoded) {
    if (c == '=') {
      break;
    }
    if (c == '+') {
      url_safe.push_back('-');
    } else if (c == '/') {
      url_safe.push_back('_');
    } else {
      url_safe.push_back(c);
    }
  }
  return url_safe;
}

Result<ContentHash> hash_token(std::string_view token) {
  auto hash = sha256(token);
  if (!hash) {
    return hash.error();
  }
  return hash.value();
}

bool constant_time_equal(const std::uint8_t* lhs, const std::uint8_t* rhs,
                         std::size_t size) noexcept {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  return CRYPTO_memcmp(lhs, rhs, size) == 0;
}

}  // namespace syncbox
