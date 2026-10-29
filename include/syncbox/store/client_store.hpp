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

struct LocalEntryRow {
  FileId file_id;
  std::string path;
  std::string path_key;
  Revision server_revision;
  std::optional<ContentHash> content_hash;
  std::optional<std::int64_t> size;
  std::optional<std::int64_t> mtime_ms;
  bool deleted = false;
  bool dirty = false;
  std::optional<std::int64_t> fs_ino;
  std::optional<std::int64_t> fs_dev;
  Seq applied_seq;
  std::optional<std::string> conflict_of;
  std::int64_t updated_at = 0;
};

struct PendingOpRow {
  OpId op_id;
  std::optional<FileId> file_id;
  OpKind kind = OpKind::Create;
  Revision base_revision;
  std::string path;
  std::optional<std::string> new_path;
  std::optional<ContentHash> content_hash;
  std::optional<std::int64_t> size;
  std::optional<std::string> staged_path;
  Seq client_seq;
  std::string state = "staged";
  int attempts = 0;
  std::optional<std::int64_t> next_attempt_at;
  std::optional<std::string> last_error;
  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;
};

struct ApplyJournalRow {
  std::int64_t id = 0;
  FileId file_id;
  std::string target_path;
  std::string temp_path;
  std::optional<ContentHash> expected_hash;
  std::optional<std::int64_t> expected_size;
  std::string state = "downloading";
  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;
};

struct ConflictRow {
  std::string conflict_id;
  FileId file_id;
  std::string kind;
  std::optional<std::string> local_path;
  std::optional<std::string> remote_path;
  std::string resolution = "manual_pending";
  std::optional<std::string> detail;
  std::int64_t created_at = 0;
  std::optional<std::int64_t> resolved_at;
};

// Data access for the client database (docs/DATABASE.md section 3).
class ClientStore {
 public:
  static Result<ClientStore> open(const std::string& path);

  [[nodiscard]] Database& db() noexcept { return db_; }
  [[nodiscard]] Result<void> migrate();

  [[nodiscard]] Result<std::optional<std::string>> get_meta(std::string_view key);
  [[nodiscard]] Result<void> set_meta(std::string_view key, std::string_view value);

  // Cursor only moves forward (invariant DB-7); older values are ignored.
  [[nodiscard]] Result<void> set_cursor(Seq cursor);
  [[nodiscard]] Result<Seq> cursor();

  [[nodiscard]] Result<void> upsert_local_entry(const LocalEntryRow& entry);
  [[nodiscard]] Result<std::optional<LocalEntryRow>> find_local_entry(const FileId& file_id);
  [[nodiscard]] Result<std::optional<LocalEntryRow>> find_local_entry_by_path_key(
      std::string_view path_key);
  [[nodiscard]] Result<std::vector<LocalEntryRow>> list_local_entries(bool include_deleted);
  [[nodiscard]] Result<void> delete_local_entry(const FileId& file_id);
  [[nodiscard]] Result<void> update_local_entry_file_id(const FileId& old_id,
                                                        const FileId& new_id);

  [[nodiscard]] Result<void> enqueue_pending_op(const PendingOpRow& op);
  [[nodiscard]] Result<std::vector<PendingOpRow>> list_pending_ops(
      std::optional<std::string> state = std::nullopt);
  [[nodiscard]] Result<void> update_pending_state(const OpId& op_id, std::string_view state,
                                                  std::optional<std::string> last_error,
                                                  std::optional<std::int64_t> next_attempt_at);
  [[nodiscard]] Result<void> update_pending_retry(const OpId& op_id, std::string_view last_error,
                                                  std::int64_t next_attempt_at);
  [[nodiscard]] Result<void> delete_pending_op(const OpId& op_id);

  [[nodiscard]] Result<std::int64_t> insert_apply_journal(const ApplyJournalRow& row);
  [[nodiscard]] Result<void> update_apply_journal_state(std::int64_t id, std::string_view state);
  [[nodiscard]] Result<std::vector<ApplyJournalRow>> list_incomplete_apply_journal();

  [[nodiscard]] Result<void> insert_conflict(const ConflictRow& conflict);
  [[nodiscard]] Result<std::vector<ConflictRow>> list_unresolved_conflicts();

 private:
  explicit ClientStore(Database db) noexcept : db_(std::move(db)) {}
  Database db_;
};

}  // namespace syncbox
