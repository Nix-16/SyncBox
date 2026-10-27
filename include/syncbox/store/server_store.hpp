#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/core/version_model.hpp>
#include <syncbox/store/database.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncbox {

struct SpaceRecord {
  std::string space_id;
  std::string name;
  std::int64_t created_at = 0;
  std::string state = "active";
};

struct DeviceRecord {
  std::string device_id;
  std::string space_id;
  std::string display_name;
  std::string platform;
  std::vector<std::uint8_t> token_hash;
  std::string status = "active";
  std::optional<std::string> app_version;
  std::int64_t created_at = 0;
  std::optional<std::int64_t> last_seen_at;
  Seq last_ack_seq;
  std::optional<std::int64_t> revoked_at;
  std::optional<std::string> revoked_reason;
};

struct EntryRow {
  FileId file_id;
  std::string path;
  std::string path_key;
  Revision revision;
  std::optional<ContentHash> content_hash;
  std::int64_t size = 0;
  std::int64_t mtime_ms = 0;
  bool deleted = false;
  std::string editor_device;
  Seq updated_seq;
  std::int64_t updated_at = 0;
};

struct VersionRecord {
  std::string space_id;
  FileId file_id;
  Revision revision;
  Revision parent_revision;
  OpKind op = OpKind::Create;
  std::string path;
  std::optional<ContentHash> content_hash;
  std::int64_t size = 0;
  std::int64_t mtime_ms = 0;
  std::string device_id;
  OpId op_id;
  std::int64_t created_at = 0;
};

struct ChangeRecord {
  Seq seq;
  std::string space_id;
  FileId file_id;
  Revision revision;
  OpKind op = OpKind::Create;
  std::string path;
  std::optional<ContentHash> content_hash;
  std::int64_t size = 0;
  bool deleted = false;
  bool conflict = false;
  std::string device_id;
  std::int64_t created_at = 0;
};

struct OperationRecord {
  OpId op_id;
  std::string space_id;
  std::string device_id;
  std::string request_hash;
  int response_code = 0;
  std::string response_body;
  Seq seq;
  std::int64_t created_at = 0;
};

struct EnrollmentTokenRecord {
  std::vector<std::uint8_t> token_hash;
  std::string space_id;
  std::string created_by;
  std::int64_t expires_at = 0;
  std::optional<std::string> used_by;
  std::optional<std::int64_t> used_at;
};

struct BlobRecord {
  std::string content_hash;
  std::int64_t size = 0;
  std::int64_t chunk_size = 0;
  int chunk_count = 0;
  std::string state = "uploading";
  int ref_count = 0;
  std::int64_t created_at = 0;
  std::optional<std::int64_t> completed_at;
};

struct BlobChunkRecord {
  std::string content_hash;
  int chunk_index = 0;
  std::string chunk_hash;
  std::int64_t size = 0;
  std::optional<std::int64_t> received_at;
};

// Data access for the server database. Business rules (CAS, idempotency,
// path policy) live in the server layer (M4); this class only reads/writes.
class ServerStore {
 public:
  static Result<ServerStore> open(const std::string& path);

  [[nodiscard]] Database& db() noexcept { return db_; }
  [[nodiscard]] Result<void> migrate();

  [[nodiscard]] Result<void> insert_space(const SpaceRecord& space);
  [[nodiscard]] Result<std::optional<SpaceRecord>> find_space(std::string_view space_id);

  [[nodiscard]] Result<void> insert_device(const DeviceRecord& device);
  [[nodiscard]] Result<std::optional<DeviceRecord>> find_device_by_token_hash(
      const std::vector<std::uint8_t>& token_hash);
  [[nodiscard]] Result<void> set_device_status(std::string_view device_id,
                                               std::string_view status,
                                               std::optional<std::string> reason);

  [[nodiscard]] Result<std::optional<EntryRow>> find_entry(std::string_view space_id,
                                                           const FileId& file_id);
  [[nodiscard]] Result<void> upsert_entry(std::string_view space_id, const EntryRow& entry);

  [[nodiscard]] Result<void> insert_version(const VersionRecord& version);
  [[nodiscard]] Result<std::optional<VersionRecord>> find_version(std::string_view space_id,
                                                                  const FileId& file_id,
                                                                  Revision revision);

  [[nodiscard]] Result<Seq> append_change(const ChangeRecord& change);
  [[nodiscard]] Result<std::vector<ChangeRecord>> list_changes(std::string_view space_id,
                                                               Seq since, int limit);
  [[nodiscard]] Result<Seq> latest_seq(std::string_view space_id);

  [[nodiscard]] Result<std::optional<OperationRecord>> find_operation(const OpId& op_id);
  [[nodiscard]] Result<void> insert_operation(const OperationRecord& operation);

  [[nodiscard]] Result<void> insert_enrollment_token(const EnrollmentTokenRecord& token);
  [[nodiscard]] Result<std::optional<EnrollmentTokenRecord>> find_enrollment_token(
      const std::vector<std::uint8_t>& token_hash);
  [[nodiscard]] Result<void> mark_enrollment_token_used(const std::vector<std::uint8_t>& token_hash,
                                                        std::string_view device_id,
                                                        std::int64_t used_at);

  [[nodiscard]] Result<std::optional<DeviceRecord>> find_device(std::string_view device_id);
  [[nodiscard]] Result<void> update_device_ack(std::string_view device_id, Seq last_seq);
  [[nodiscard]] Result<void> update_device_last_seen(std::string_view device_id,
                                                     std::int64_t seen_at);

  [[nodiscard]] Result<std::optional<EntryRow>> find_live_entry_by_path_key(
      std::string_view space_id, std::string_view path_key);
  [[nodiscard]] Result<std::optional<EntryRow>> find_tombstone_by_path_key(
      std::string_view space_id, std::string_view path_key);
  // after_token is empty for the first page, or "<path_key>\x1f<file_id>".
  [[nodiscard]] Result<std::vector<EntryRow>> list_snapshot(std::string_view space_id,
                                                            std::string_view after_token,
                                                            int limit);

  [[nodiscard]] Result<std::optional<BlobRecord>> find_blob(std::string_view content_hash);
  [[nodiscard]] Result<void> upsert_uploading_blob(const BlobRecord& blob);
  [[nodiscard]] Result<void> insert_blob_chunk(const BlobChunkRecord& chunk);
  [[nodiscard]] Result<std::optional<BlobChunkRecord>> find_blob_chunk(
      std::string_view content_hash, int chunk_index);
  [[nodiscard]] Result<void> mark_blob_chunk_received(std::string_view content_hash,
                                                      int chunk_index,
                                                      std::int64_t received_at);
  [[nodiscard]] Result<std::vector<int>> list_missing_chunks(std::string_view content_hash);
  [[nodiscard]] Result<void> mark_blob_complete(std::string_view content_hash,
                                                std::int64_t completed_at);
  [[nodiscard]] Result<void> increment_blob_ref(std::string_view content_hash);
  [[nodiscard]] Result<bool> blob_referenced_in_space(std::string_view space_id,
                                                      std::string_view content_hash);

 private:
  explicit ServerStore(Database db) noexcept : db_(std::move(db)) {}
  Database db_;
};

}  // namespace syncbox
