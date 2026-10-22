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

  [[nodiscard]] Result<std::optional<OperationRecord>> find_operation(const OpId& op_id);
  [[nodiscard]] Result<void> insert_operation(const OperationRecord& operation);

 private:
  explicit ServerStore(Database db) noexcept : db_(std::move(db)) {}
  Database db_;
};

}  // namespace syncbox
