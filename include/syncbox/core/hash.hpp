#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/core/version_model.hpp>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace syncbox {

// Streaming SHA-256 built on OpenSSL EVP (no deprecated SHA256_* APIs).
class Sha256Hasher {
 public:
  Sha256Hasher();
  ~Sha256Hasher();

  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  Sha256Hasher(Sha256Hasher&& other) noexcept;
  Sha256Hasher& operator=(Sha256Hasher&& other) noexcept;

  [[nodiscard]] Result<void> update(const void* data, std::size_t size);
  [[nodiscard]] Result<ContentHash> finalize();

 private:
  void* context_ = nullptr;  // EVP_MD_CTX*, hidden to keep OpenSSL out of this header
};

[[nodiscard]] Result<ContentHash> sha256(std::string_view data);

// Streams the file in 64 KiB blocks; never loads the whole file into memory.
[[nodiscard]] Result<ContentHash> sha256_file(const std::filesystem::path& path);

}  // namespace syncbox
