#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/store/server_store.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syncbox::server {

struct BlobInitResult {
  bool complete = false;
  std::vector<int> missing_chunks;
};

// Content-addressed chunk storage. Chunks live in
// <root>/<sha256>/<index>.chunk and metadata lives in the blobs tables.
class BlobStore {
 public:
  BlobStore(ServerStore& store, std::filesystem::path root)
      : store_(store), root_(std::move(root)) {}

  [[nodiscard]] Result<void> prepare();

  [[nodiscard]] Result<BlobInitResult> init_upload(
      std::string_view content_hash, std::int64_t size, std::int64_t chunk_size,
      const std::vector<std::string>& chunk_hashes);

  [[nodiscard]] Result<void> put_chunk(std::string_view content_hash, int chunk_index,
                                       std::string_view data, std::string_view chunk_hash);

  [[nodiscard]] Result<BlobInitResult> status(std::string_view content_hash);

  // Verifies the assembled SHA-256 before marking the blob complete.
  [[nodiscard]] Result<void> complete(std::string_view content_hash);

  [[nodiscard]] Result<std::int64_t> size_of(std::string_view content_hash);

  // Reads [offset, offset+length). length < 0 means "to the end".
  [[nodiscard]] Result<std::string> read_range(std::string_view content_hash,
                                               std::int64_t offset, std::int64_t length);

 private:
  [[nodiscard]] std::filesystem::path chunk_path(const std::string& content_hash,
                                                 int chunk_index) const;
  [[nodiscard]] Result<std::string> read_chunk_file(const std::string& content_hash,
                                                    int chunk_index) const;

  ServerStore& store_;
  std::filesystem::path root_;
};

}  // namespace syncbox::server
