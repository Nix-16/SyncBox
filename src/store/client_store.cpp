#include <syncbox/store/client_store.hpp>

#include <syncbox/store/migrations.hpp>

#include <chrono>
#include <exception>
#include <utility>

namespace syncbox {

namespace {

[[nodiscard]] std::int64_t now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] Result<void> bind_optional_text(Statement& statement, int index,
                                              const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return statement.bind_null(index);
  }
  return statement.bind_text(index, *value);
}

[[nodiscard]] Result<void> bind_optional_int64(Statement& statement, int index,
                                               const std::optional<std::int64_t>& value) {
  if (!value.has_value()) {
    return statement.bind_null(index);
  }
  return statement.bind_int64(index, *value);
}

[[nodiscard]] Result<void> bind_optional_hash(Statement& statement, int index,
                                              const std::optional<ContentHash>& value) {
  if (!value.has_value()) {
    return statement.bind_null(index);
  }
  return statement.bind_text(index, value->to_hex());
}

[[nodiscard]] std::optional<ContentHash> read_optional_hash(const Statement& statement, int column) {
  if (statement.column_is_null(column)) {
    return std::nullopt;
  }
  return ContentHash::from_hex(statement.column_text(column));
}

[[nodiscard]] std::optional<std::string> read_optional_text(const Statement& statement, int column) {
  if (statement.column_is_null(column)) {
    return std::nullopt;
  }
  return statement.column_text(column);
}

[[nodiscard]] std::optional<std::int64_t> read_optional_int64(const Statement& statement,
                                                             int column) {
  if (statement.column_is_null(column)) {
    return std::nullopt;
  }
  return statement.column_int64(column);
}

}  // namespace

Result<ClientStore> ClientStore::open(const std::string& path) {
  DatabaseOptions options;
  options.mode = DatabaseMode::Client;
  auto database = Database::open(path, options);
  if (!database) {
    return database.error();
  }
  return ClientStore(std::move(database).value());
}

Result<void> ClientStore::migrate() {
  return migrate_client(db_);
}

Result<std::optional<std::string>> ClientStore::get_meta(std::string_view key) {
  auto statement = Statement::prepare(db_, "SELECT value FROM meta WHERE key=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, key); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{stmt.column_text(0)};
}

Result<void> ClientStore::set_meta(std::string_view key, std::string_view value) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO meta(key,value) VALUES(?,?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, key); !r) return r.error();
  if (auto r = stmt.bind_text(2, value); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<void> ClientStore::set_cursor(Seq cursor_value) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO meta(key,value) VALUES('cursor',?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value "
      "WHERE CAST(meta.value AS INTEGER) <= CAST(excluded.value AS INTEGER);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, std::to_string(cursor_value.value)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<Seq> ClientStore::cursor() {
  auto value = get_meta("cursor");
  if (!value) {
    return value.error();
  }
  if (!value.value().has_value()) {
    return Seq{0};
  }
  try {
    return Seq{std::stoll(*value.value())};
  } catch (const std::exception&) {
    return make_error(ErrorCode::DatabaseError, "cursor is not a valid integer");
  }
}

Result<void> ClientStore::upsert_local_entry(const LocalEntryRow& entry) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO local_entries(file_id,path,path_key,server_revision,content_hash,size,"
      "mtime_ms,deleted,dirty,fs_ino,fs_dev,applied_seq,conflict_of,updated_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(file_id) DO UPDATE SET path=excluded.path,path_key=excluded.path_key,"
      "server_revision=excluded.server_revision,content_hash=excluded.content_hash,"
      "size=excluded.size,mtime_ms=excluded.mtime_ms,deleted=excluded.deleted,"
      "dirty=excluded.dirty,fs_ino=excluded.fs_ino,fs_dev=excluded.fs_dev,"
      "applied_seq=excluded.applied_seq,conflict_of=excluded.conflict_of,"
      "updated_at=excluded.updated_at;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, entry.file_id.value); !r) return r.error();
  if (auto r = stmt.bind_text(2, entry.path); !r) return r.error();
  if (auto r = stmt.bind_text(3, entry.path_key); !r) return r.error();
  if (auto r = stmt.bind_int64(4, entry.server_revision.value); !r) return r.error();
  if (auto r = bind_optional_hash(stmt, 5, entry.content_hash); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 6, entry.size); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 7, entry.mtime_ms); !r) return r.error();
  if (auto r = stmt.bind_int64(8, entry.deleted ? 1 : 0); !r) return r.error();
  if (auto r = stmt.bind_int64(9, entry.dirty ? 1 : 0); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 10, entry.fs_ino); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 11, entry.fs_dev); !r) return r.error();
  if (auto r = stmt.bind_int64(12, entry.applied_seq.value); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 13, entry.conflict_of); !r) return r.error();
  if (auto r = stmt.bind_int64(14, entry.updated_at); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::optional<LocalEntryRow>> ClientStore::find_local_entry(const FileId& file_id) {
  auto statement = Statement::prepare(
      db_,
      "SELECT file_id,path,path_key,server_revision,content_hash,size,mtime_ms,deleted,dirty,"
      "fs_ino,fs_dev,applied_seq,conflict_of,updated_at FROM local_entries WHERE file_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, file_id.value); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<LocalEntryRow>{};
  }

  LocalEntryRow entry;
  entry.file_id = FileId{stmt.column_text(0)};
  entry.path = stmt.column_text(1);
  entry.path_key = stmt.column_text(2);
  entry.server_revision = Revision{stmt.column_int64(3)};
  entry.content_hash = read_optional_hash(stmt, 4);
  entry.size = read_optional_int64(stmt, 5);
  entry.mtime_ms = read_optional_int64(stmt, 6);
  entry.deleted = stmt.column_int64(7) != 0;
  entry.dirty = stmt.column_int64(8) != 0;
  entry.fs_ino = read_optional_int64(stmt, 9);
  entry.fs_dev = read_optional_int64(stmt, 10);
  entry.applied_seq = Seq{stmt.column_int64(11)};
  entry.conflict_of = read_optional_text(stmt, 12);
  entry.updated_at = stmt.column_int64(13);
  return std::optional<LocalEntryRow>{std::move(entry)};
}

Result<std::optional<LocalEntryRow>> ClientStore::find_local_entry_by_path_key(
    std::string_view path_key) {
  auto statement = Statement::prepare(
      db_,
      "SELECT file_id,path,path_key,server_revision,content_hash,size,mtime_ms,deleted,dirty,"
      "fs_ino,fs_dev,applied_seq,conflict_of,updated_at FROM local_entries "
      "WHERE path_key=? AND deleted=0 ORDER BY file_id LIMIT 1;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, path_key); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<LocalEntryRow>{};
  }

  LocalEntryRow entry;
  entry.file_id = FileId{stmt.column_text(0)};
  entry.path = stmt.column_text(1);
  entry.path_key = stmt.column_text(2);
  entry.server_revision = Revision{stmt.column_int64(3)};
  entry.content_hash = read_optional_hash(stmt, 4);
  entry.size = read_optional_int64(stmt, 5);
  entry.mtime_ms = read_optional_int64(stmt, 6);
  entry.deleted = stmt.column_int64(7) != 0;
  entry.dirty = stmt.column_int64(8) != 0;
  entry.fs_ino = read_optional_int64(stmt, 9);
  entry.fs_dev = read_optional_int64(stmt, 10);
  entry.applied_seq = Seq{stmt.column_int64(11)};
  entry.conflict_of = read_optional_text(stmt, 12);
  entry.updated_at = stmt.column_int64(13);
  return std::optional<LocalEntryRow>{std::move(entry)};
}

Result<std::vector<LocalEntryRow>> ClientStore::list_local_entries(bool include_deleted) {
  const char* sql =
      include_deleted
          ? "SELECT file_id,path,path_key,server_revision,content_hash,size,mtime_ms,deleted,"
            "dirty,fs_ino,fs_dev,applied_seq,conflict_of,updated_at FROM local_entries "
            "ORDER BY path;"
          : "SELECT file_id,path,path_key,server_revision,content_hash,size,mtime_ms,deleted,"
            "dirty,fs_ino,fs_dev,applied_seq,conflict_of,updated_at FROM local_entries "
            "WHERE deleted=0 ORDER BY path;";
  auto statement = Statement::prepare(db_, sql);
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();

  std::vector<LocalEntryRow> entries;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      break;
    }
    LocalEntryRow entry;
    entry.file_id = FileId{stmt.column_text(0)};
    entry.path = stmt.column_text(1);
    entry.path_key = stmt.column_text(2);
    entry.server_revision = Revision{stmt.column_int64(3)};
    entry.content_hash = read_optional_hash(stmt, 4);
    entry.size = read_optional_int64(stmt, 5);
    entry.mtime_ms = read_optional_int64(stmt, 6);
    entry.deleted = stmt.column_int64(7) != 0;
    entry.dirty = stmt.column_int64(8) != 0;
    entry.fs_ino = read_optional_int64(stmt, 9);
    entry.fs_dev = read_optional_int64(stmt, 10);
    entry.applied_seq = Seq{stmt.column_int64(11)};
    entry.conflict_of = read_optional_text(stmt, 12);
    entry.updated_at = stmt.column_int64(13);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> ClientStore::enqueue_pending_op(const PendingOpRow& op) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO pending_ops(op_id,file_id,kind,base_revision,path,new_path,content_hash,"
      "size,staged_path,client_seq,state,attempts,next_attempt_at,last_error,created_at,"
      "updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, op.op_id.value); !r) return r.error();
  if (auto r = op.file_id.has_value() ? stmt.bind_text(2, op.file_id->value) : stmt.bind_null(2);
      !r) {
    return r.error();
  }
  if (auto r = stmt.bind_text(3, to_string(op.kind)); !r) return r.error();
  if (auto r = stmt.bind_int64(4, op.base_revision.value); !r) return r.error();
  if (auto r = stmt.bind_text(5, op.path); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 6, op.new_path); !r) return r.error();
  if (auto r = bind_optional_hash(stmt, 7, op.content_hash); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 8, op.size); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 9, op.staged_path); !r) return r.error();
  if (auto r = stmt.bind_int64(10, op.client_seq.value); !r) return r.error();
  if (auto r = stmt.bind_text(11, op.state); !r) return r.error();
  if (auto r = stmt.bind_int64(12, op.attempts); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 13, op.next_attempt_at); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 14, op.last_error); !r) return r.error();
  if (auto r = stmt.bind_int64(15, op.created_at); !r) return r.error();
  if (auto r = stmt.bind_int64(16, op.updated_at); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::vector<PendingOpRow>> ClientStore::list_pending_ops(std::optional<std::string> state) {
  const char* sql =
      state.has_value()
          ? "SELECT op_id,file_id,kind,base_revision,path,new_path,content_hash,size,"
            "staged_path,client_seq,state,attempts,next_attempt_at,last_error,created_at,"
            "updated_at FROM pending_ops WHERE state=? ORDER BY created_at,op_id;"
          : "SELECT op_id,file_id,kind,base_revision,path,new_path,content_hash,size,"
            "staged_path,client_seq,state,attempts,next_attempt_at,last_error,created_at,"
            "updated_at FROM pending_ops ORDER BY created_at,op_id;";
  auto statement = Statement::prepare(db_, sql);
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (state.has_value()) {
    if (auto r = stmt.bind_text(1, *state); !r) return r.error();
  }

  std::vector<PendingOpRow> ops;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      break;
    }
    PendingOpRow op;
    op.op_id = OpId{stmt.column_text(0)};
    if (!stmt.column_is_null(1)) {
      op.file_id = FileId{stmt.column_text(1)};
    }
    const auto kind = op_kind_from_string(stmt.column_text(2));
    if (!kind.has_value()) {
      return make_error(ErrorCode::DatabaseError, "invalid kind value in pending_ops");
    }
    op.kind = *kind;
    op.base_revision = Revision{stmt.column_int64(3)};
    op.path = stmt.column_text(4);
    op.new_path = read_optional_text(stmt, 5);
    op.content_hash = read_optional_hash(stmt, 6);
    op.size = read_optional_int64(stmt, 7);
    op.staged_path = read_optional_text(stmt, 8);
    op.client_seq = Seq{stmt.column_int64(9)};
    op.state = stmt.column_text(10);
    op.attempts = static_cast<int>(stmt.column_int64(11));
    op.next_attempt_at = read_optional_int64(stmt, 12);
    op.last_error = read_optional_text(stmt, 13);
    op.created_at = stmt.column_int64(14);
    op.updated_at = stmt.column_int64(15);
    ops.push_back(std::move(op));
  }
  return ops;
}

Result<void> ClientStore::update_pending_state(const OpId& op_id, std::string_view state,
                                               std::optional<std::string> last_error,
                                               std::optional<std::int64_t> next_attempt_at) {
  auto statement = Statement::prepare(
      db_,
      "UPDATE pending_ops SET state=?,last_error=?,next_attempt_at=?,updated_at=? WHERE op_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, state); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 2, last_error); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 3, next_attempt_at); !r) return r.error();
  if (auto r = stmt.bind_int64(4, now_millis()); !r) return r.error();
  if (auto r = stmt.bind_text(5, op_id.value); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<void> ClientStore::delete_pending_op(const OpId& op_id) {
  auto statement = Statement::prepare(db_, "DELETE FROM pending_ops WHERE op_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, op_id.value); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::int64_t> ClientStore::insert_apply_journal(const ApplyJournalRow& row) {
  const auto now = now_millis();
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO apply_journal(file_id,target_path,temp_path,expected_hash,expected_size,"
      "state,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, row.file_id.value); !r) return r.error();
  if (auto r = stmt.bind_text(2, row.target_path); !r) return r.error();
  if (auto r = stmt.bind_text(3, row.temp_path); !r) return r.error();
  if (auto r = bind_optional_hash(stmt, 4, row.expected_hash); !r) return r.error();
  if (auto r = bind_optional_int64(stmt, 5, row.expected_size); !r) return r.error();
  if (auto r = stmt.bind_text(6, row.state); !r) return r.error();
  if (auto r = stmt.bind_int64(7, now); !r) return r.error();
  if (auto r = stmt.bind_int64(8, now); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  auto rowid = db_.last_insert_rowid();
  if (!rowid) {
    return rowid.error();
  }
  return rowid;
}

Result<void> ClientStore::update_apply_journal_state(std::int64_t id, std::string_view state) {
  auto statement =
      Statement::prepare(db_, "UPDATE apply_journal SET state=?,updated_at=? WHERE id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, state); !r) return r.error();
  if (auto r = stmt.bind_int64(2, now_millis()); !r) return r.error();
  if (auto r = stmt.bind_int64(3, id); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::vector<ApplyJournalRow>> ClientStore::list_incomplete_apply_journal() {
  auto statement = Statement::prepare(
      db_,
      "SELECT id,file_id,target_path,temp_path,expected_hash,expected_size,state,created_at,"
      "updated_at FROM apply_journal WHERE state NOT IN ('done','failed') ORDER BY id;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();

  std::vector<ApplyJournalRow> rows;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      break;
    }
    ApplyJournalRow row;
    row.id = stmt.column_int64(0);
    row.file_id = FileId{stmt.column_text(1)};
    row.target_path = stmt.column_text(2);
    row.temp_path = stmt.column_text(3);
    row.expected_hash = read_optional_hash(stmt, 4);
    row.expected_size = read_optional_int64(stmt, 5);
    row.state = stmt.column_text(6);
    row.created_at = stmt.column_int64(7);
    row.updated_at = stmt.column_int64(8);
    rows.push_back(std::move(row));
  }
  return rows;
}

Result<void> ClientStore::insert_conflict(const ConflictRow& conflict) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO conflicts(conflict_id,file_id,kind,local_path,remote_path,resolution,detail,"
      "created_at,resolved_at) VALUES(?,?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = stmt.bind_text(1, conflict.conflict_id); !r) return r.error();
  if (auto r = stmt.bind_text(2, conflict.file_id.value); !r) return r.error();
  if (auto r = stmt.bind_text(3, conflict.kind); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 4, conflict.local_path); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 5, conflict.remote_path); !r) return r.error();
  if (auto r = stmt.bind_text(6, conflict.resolution); !r) return r.error();
  if (auto r = bind_optional_text(stmt, 7, conflict.detail); !r) return r.error();
  if (auto r = stmt.bind_int64(8, conflict.created_at != 0 ? conflict.created_at : now_millis());
      !r) {
    return r.error();
  }
  if (auto r = bind_optional_int64(stmt, 9, conflict.resolved_at); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::vector<ConflictRow>> ClientStore::list_unresolved_conflicts() {
  auto statement = Statement::prepare(
      db_,
      "SELECT conflict_id,file_id,kind,local_path,remote_path,resolution,detail,created_at,"
      "resolved_at FROM conflicts WHERE resolved_at IS NULL ORDER BY created_at;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();

  std::vector<ConflictRow> conflicts;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      break;
    }
    ConflictRow conflict;
    conflict.conflict_id = stmt.column_text(0);
    conflict.file_id = FileId{stmt.column_text(1)};
    conflict.kind = stmt.column_text(2);
    conflict.local_path = read_optional_text(stmt, 3);
    conflict.remote_path = read_optional_text(stmt, 4);
    conflict.resolution = stmt.column_text(5);
    conflict.detail = read_optional_text(stmt, 6);
    conflict.created_at = stmt.column_int64(7);
    conflict.resolved_at = read_optional_int64(stmt, 8);
    conflicts.push_back(std::move(conflict));
  }
  return conflicts;
}

}  // namespace syncbox
