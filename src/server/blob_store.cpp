#include <syncbox/server/blob_store.hpp>

#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/uuid.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace syncbox::server {

namespace {

[[nodiscard]] Result<std::string> canonical_hash(std::string_view text) {
  auto hash = ContentHash::from_hex(text);
  if (!hash.has_value()) {
    return make_error(ErrorCode::BadRequest, "content_hash must be 64 hex characters");
  }
  return hash->to_hex();
}

[[nodiscard]] int chunk_count_for(std::int64_t size, std::int64_t chunk_size) {
  if (size <= 0) {
    return 1;
  }
  return static_cast<int>(size / chunk_size + (size % chunk_size == 0 ? 0 : 1));
}

[[nodiscard]] std::int64_t chunk_size_at(std::int64_t size, std::int64_t chunk_size, int index) {
  if (size <= 0) {
    return 0;
  }
  const std::int64_t start = static_cast<std::int64_t>(index) * chunk_size;
  if (start >= size) {
    return 0;
  }
  return std::min(chunk_size, size - start);
}

}  // namespace

Result<void> BlobStore::prepare() {
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create blob root: " + error.message());
  }
  return Result<void>{};
}

std::filesystem::path BlobStore::chunk_path(const std::string& content_hash,
                                            int chunk_index) const {
  return root_ / content_hash / (std::to_string(chunk_index) + ".chunk");
}

Result<BlobInitResult> BlobStore::init_upload(std::string_view content_hash_text,
                                              std::int64_t size, std::int64_t chunk_size,
                                              const std::vector<std::string>& chunk_hashes) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  if (size < 0 || chunk_size <= 0) {
    return make_error(ErrorCode::BadRequest, "size and chunk_size must be positive");
  }

  const int chunk_count = chunk_count_for(size, chunk_size);
  if (static_cast<int>(chunk_hashes.size()) != chunk_count) {
    return make_error(ErrorCode::BadRequest, "chunk_hashes length does not match chunk_count");
  }
  for (const auto& chunk_hash : chunk_hashes) {
    if (!ContentHash::from_hex(chunk_hash).has_value()) {
      return make_error(ErrorCode::BadRequest, "chunk hash must be 64 hex characters");
    }
  }

  auto existing = store_.find_blob(hash.value());
  if (!existing) {
    return existing.error();
  }
  if (existing.value().has_value() && existing.value()->state == "complete") {
    if (existing.value()->size != size) {
      return make_error(ErrorCode::BadRequest, "size does not match the existing blob");
    }
    BlobInitResult result;
    result.complete = true;
    return result;
  }

  BlobRecord blob;
  blob.content_hash = hash.value();
  blob.size = size;
  blob.chunk_size = chunk_size;
  blob.chunk_count = chunk_count;
  blob.created_at = now_millis();
  auto upserted = store_.upsert_uploading_blob(blob);
  if (!upserted) {
    return upserted.error();
  }

  for (int index = 0; index < chunk_count; ++index) {
    BlobChunkRecord chunk;
    chunk.content_hash = hash.value();
    chunk.chunk_index = index;
    chunk.chunk_hash = ContentHash::from_hex(chunk_hashes[static_cast<std::size_t>(index)])
                           ->to_hex();
    chunk.size = chunk_size_at(size, chunk_size, index);
    auto inserted = store_.insert_blob_chunk(chunk);
    if (!inserted) {
      return inserted.error();
    }
  }

  return status(hash.value());
}

Result<BlobInitResult> BlobStore::status(std::string_view content_hash_text) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  auto blob = store_.find_blob(hash.value());
  if (!blob) {
    return blob.error();
  }
  if (!blob.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "blob does not exist");
  }

  BlobInitResult result;
  result.complete = blob.value()->state == "complete";
  if (!result.complete) {
    auto missing = store_.list_missing_chunks(hash.value());
    if (!missing) {
      return missing.error();
    }
    result.missing_chunks = missing.value();
  }
  return result;
}

Result<void> BlobStore::put_chunk(std::string_view content_hash_text, int chunk_index,
                                  std::string_view data, std::string_view chunk_hash_text) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  auto provided = canonical_hash(chunk_hash_text);
  if (!provided) {
    return provided.error();
  }

  auto blob = store_.find_blob(hash.value());
  if (!blob) {
    return blob.error();
  }
  if (!blob.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "blob does not exist");
  }
  if (blob.value()->state == "complete") {
    return Result<void>{};
  }
  if (chunk_index < 0 || chunk_index >= blob.value()->chunk_count) {
    return make_error(ErrorCode::BadRequest, "chunk index is out of range");
  }

  auto chunk = store_.find_blob_chunk(hash.value(), chunk_index);
  if (!chunk) {
    return chunk.error();
  }
  if (!chunk.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "chunk metadata does not exist");
  }
  if (static_cast<std::int64_t>(data.size()) != chunk.value()->size) {
    return make_error(ErrorCode::BadRequest, "chunk size does not match the manifest");
  }

  auto digest = sha256(data);
  if (!digest) {
    return digest.error();
  }
  const std::string actual = digest.value().to_hex();
  if (actual != provided.value() || actual != chunk.value()->chunk_hash) {
    return make_error(ErrorCode::ChunkHashMismatch, "chunk hash does not match");
  }

  std::error_code error;
  const auto path = chunk_path(hash.value(), chunk_index);
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create blob directory: " + error.message());
  }

  const auto temporary = path.parent_path() / (path.filename().string() + ".tmp." +
                                               generate_uuid_v7());
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return make_error(ErrorCode::IoError, "cannot open chunk temp file");
    }
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.flush();
    if (!stream) {
      std::filesystem::remove(temporary, error);
      return make_error(ErrorCode::IoError, "cannot write chunk");
    }
  }

  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return make_error(ErrorCode::IoError, "cannot store chunk: " + error.message());
  }

  return store_.mark_blob_chunk_received(hash.value(), chunk_index, now_millis());
}

Result<std::string> BlobStore::read_chunk_file(const std::string& content_hash,
                                               int chunk_index) const {
  std::ifstream stream(chunk_path(content_hash, chunk_index), std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoError, "cannot open chunk file");
  }
  std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (stream.bad()) {
    return make_error(ErrorCode::IoError, "cannot read chunk file");
  }
  return data;
}

Result<void> BlobStore::complete(std::string_view content_hash_text) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  auto blob = store_.find_blob(hash.value());
  if (!blob) {
    return blob.error();
  }
  if (!blob.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "blob does not exist");
  }
  if (blob.value()->state == "complete") {
    return Result<void>{};
  }

  auto missing = store_.list_missing_chunks(hash.value());
  if (!missing) {
    return missing.error();
  }
  if (!missing.value().empty()) {
    return make_error(ErrorCode::BlobIncomplete, "blob has missing chunks");
  }

  Sha256Hasher hasher;
  for (int index = 0; index < blob.value()->chunk_count; ++index) {
    auto data = read_chunk_file(hash.value(), index);
    if (!data) {
      return data.error();
    }
    auto updated = hasher.update(data.value().data(), data.value().size());
    if (!updated) {
      return updated.error();
    }
  }
  auto digest = hasher.finalize();
  if (!digest) {
    return digest.error();
  }
  if (digest.value().to_hex() != hash.value()) {
    return make_error(ErrorCode::BlobHashMismatch,
                      "assembled content does not match content_hash");
  }

  return store_.mark_blob_complete(hash.value(), now_millis());
}

Result<std::int64_t> BlobStore::size_of(std::string_view content_hash_text) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  auto blob = store_.find_blob(hash.value());
  if (!blob) {
    return blob.error();
  }
  if (!blob.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "blob does not exist");
  }
  return blob.value()->size;
}

Result<std::string> BlobStore::read_range(std::string_view content_hash_text, std::int64_t offset,
                                          std::int64_t length) {
  auto hash = canonical_hash(content_hash_text);
  if (!hash) {
    return hash.error();
  }
  auto blob = store_.find_blob(hash.value());
  if (!blob) {
    return blob.error();
  }
  if (!blob.value().has_value()) {
    return make_error(ErrorCode::BlobNotFound, "blob does not exist");
  }
  if (blob.value()->state != "complete") {
    return make_error(ErrorCode::BlobIncomplete, "blob is not complete");
  }

  const std::int64_t size = blob.value()->size;
  if (offset < 0 || offset > size) {
    return make_error(ErrorCode::BadRequest, "range offset is out of bounds");
  }
  std::int64_t end = size;
  if (length >= 0) {
    end = std::min(size, offset + length);
  }
  if (end <= offset) {
    return std::string{};
  }

  const std::int64_t chunk_size = blob.value()->chunk_size;
  std::string output;
  output.reserve(static_cast<std::size_t>(end - offset));

  std::int64_t position = offset;
  while (position < end) {
    const int index = static_cast<int>(position / chunk_size);
    auto data = read_chunk_file(hash.value(), index);
    if (!data) {
      return data.error();
    }
    const std::int64_t chunk_start = static_cast<std::int64_t>(index) * chunk_size;
    const std::int64_t within = position - chunk_start;
    if (within >= static_cast<std::int64_t>(data.value().size())) {
      return make_error(ErrorCode::IoError, "chunk file is shorter than expected");
    }
    const std::int64_t available = static_cast<std::int64_t>(data.value().size()) - within;
    const std::int64_t take = std::min(end - position, available);
    output.append(data.value(), static_cast<std::size_t>(within), static_cast<std::size_t>(take));
    position += take;
  }
  return output;
}

}  // namespace syncbox::server
