#include <syncbox/core/hash.hpp>

#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <utility>

namespace syncbox {

namespace {

constexpr std::size_t kReadBlockSize = 64 * 1024;

[[nodiscard]] EVP_MD_CTX* as_context(void* pointer) noexcept {
  return static_cast<EVP_MD_CTX*>(pointer);
}

}  // namespace

Sha256Hasher::Sha256Hasher() {
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return;
  }
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(context);
    return;
  }
  context_ = context;
}

Sha256Hasher::~Sha256Hasher() {
  if (context_ != nullptr) {
    EVP_MD_CTX_free(as_context(context_));
  }
}

Sha256Hasher::Sha256Hasher(Sha256Hasher&& other) noexcept : context_(other.context_) {
  other.context_ = nullptr;
}

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&& other) noexcept {
  if (this != &other) {
    if (context_ != nullptr) {
      EVP_MD_CTX_free(as_context(context_));
    }
    context_ = other.context_;
    other.context_ = nullptr;
  }
  return *this;
}

Result<void> Sha256Hasher::update(const void* data, std::size_t size) {
  if (context_ == nullptr) {
    return make_error(ErrorCode::Internal, "SHA-256 context is not initialized");
  }
  if (size == 0) {
    return Result<void>{};
  }
  if (EVP_DigestUpdate(as_context(context_), data, size) != 1) {
    return make_error(ErrorCode::Internal, "EVP_DigestUpdate failed");
  }
  return Result<void>{};
}

Result<ContentHash> Sha256Hasher::finalize() {
  if (context_ == nullptr) {
    return make_error(ErrorCode::Internal, "SHA-256 context is not initialized");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(as_context(context_), digest.data(), &digest_length) != 1) {
    EVP_MD_CTX_free(as_context(context_));
    context_ = nullptr;
    return make_error(ErrorCode::Internal, "EVP_DigestFinal_ex failed");
  }

  EVP_MD_CTX_free(as_context(context_));
  context_ = nullptr;

  if (digest_length != 32) {
    return make_error(ErrorCode::Internal, "unexpected SHA-256 digest length");
  }

  ContentHash hash;
  for (std::size_t i = 0; i < hash.bytes.size(); ++i) {
    hash.bytes[i] = static_cast<std::uint8_t>(digest[i]);
  }
  return hash;
}

Result<ContentHash> sha256(std::string_view data) {
  Sha256Hasher hasher;
  auto update_result = hasher.update(data.data(), data.size());
  if (!update_result) {
    return update_result.error();
  }
  return hasher.finalize();
}

Result<ContentHash> sha256_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoError, "cannot open file for hashing: " + path.string());
  }

  Sha256Hasher hasher;
  std::array<char, kReadBlockSize> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = stream.gcount();
    if (read_count > 0) {
      auto update_result =
          hasher.update(buffer.data(), static_cast<std::size_t>(read_count));
      if (!update_result) {
        return update_result.error();
      }
    }
  }

  if (!stream.eof()) {
    return make_error(ErrorCode::IoError, "error while reading file: " + path.string());
  }
  return hasher.finalize();
}

}  // namespace syncbox
