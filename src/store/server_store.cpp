#include <syncbox/store/server_store.hpp>

#include <syncbox/store/migrations.hpp>

#include <chrono>
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

[[nodiscard]] Result<void> check(Result<void> result) {
  return result;
}

}  // namespace

Result<ServerStore> ServerStore::open(const std::string& path) {
  DatabaseOptions options;
  options.mode = DatabaseMode::Server;
  auto database = Database::open(path, options);
  if (!database) {
    return database.error();
  }
  return ServerStore(std::move(database).value());
}

Result<void> ServerStore::migrate() {
  return migrate_server(db_);
}

Result<void> ServerStore::insert_space(const SpaceRecord& space) {
  auto statement = Statement::prepare(
      db_, "INSERT INTO spaces(space_id,name,created_at,state) VALUES(?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space.space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, space.name)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(3, space.created_at)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, space.state)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::optional<SpaceRecord>> ServerStore::find_space(std::string_view space_id) {
  auto statement = Statement::prepare(
      db_, "SELECT space_id,name,created_at,state FROM spaces WHERE space_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space_id)); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<SpaceRecord>{};
  }

  SpaceRecord space;
  space.space_id = stmt.column_text(0);
  space.name = stmt.column_text(1);
  space.created_at = stmt.column_int64(2);
  space.state = stmt.column_text(3);
  return std::optional<SpaceRecord>{std::move(space)};
}

Result<void> ServerStore::insert_device(const DeviceRecord& device) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO devices(device_id,space_id,display_name,platform,token_hash,status,"
      "app_version,created_at,last_seen_at,last_ack_seq,revoked_at,revoked_reason) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, device.device_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, device.space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(3, device.display_name)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, device.platform)); !r) return r.error();
  if (auto r = check(stmt.bind_blob(5, device.token_hash.data(),
                                    static_cast<int>(device.token_hash.size()))); !r) return r.error();
  if (auto r = check(stmt.bind_text(6, device.status)); !r) return r.error();
  if (auto r = check(bind_optional_text(stmt, 7, device.app_version)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(8, device.created_at)); !r) return r.error();
  if (auto r = check(bind_optional_int64(stmt, 9, device.last_seen_at)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(10, device.last_ack_seq.value)); !r) return r.error();
  if (auto r = check(bind_optional_int64(stmt, 11, device.revoked_at)); !r) return r.error();
  if (auto r = check(bind_optional_text(stmt, 12, device.revoked_reason)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::optional<DeviceRecord>> ServerStore::find_device_by_token_hash(
    const std::vector<std::uint8_t>& token_hash) {
  auto statement = Statement::prepare(
      db_,
      "SELECT device_id,space_id,display_name,platform,status,app_version,created_at,"
      "last_seen_at,last_ack_seq,revoked_at,revoked_reason,token_hash "
      "FROM devices WHERE token_hash=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_blob(1, token_hash.data(), static_cast<int>(token_hash.size()))); !r) {
    return r.error();
  }

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<DeviceRecord>{};
  }

  DeviceRecord device;
  device.device_id = stmt.column_text(0);
  device.space_id = stmt.column_text(1);
  device.display_name = stmt.column_text(2);
  device.platform = stmt.column_text(3);
  device.status = stmt.column_text(4);
  if (!stmt.column_is_null(5)) {
    device.app_version = stmt.column_text(5);
  }
  device.created_at = stmt.column_int64(6);
  if (!stmt.column_is_null(7)) {
    device.last_seen_at = stmt.column_int64(7);
  }
  device.last_ack_seq = Seq{stmt.column_int64(8)};
  if (!stmt.column_is_null(9)) {
    device.revoked_at = stmt.column_int64(9);
  }
  if (!stmt.column_is_null(10)) {
    device.revoked_reason = stmt.column_text(10);
  }
  return std::optional<DeviceRecord>{std::move(device)};
}

Result<void> ServerStore::set_device_status(std::string_view device_id, std::string_view status,
                                            std::optional<std::string> reason) {
  auto statement = Statement::prepare(
      db_, "UPDATE devices SET status=?, revoked_at=?, revoked_reason=? WHERE device_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  const bool revoking = (status == "revoked");
  if (auto r = check(stmt.bind_text(1, status)); !r) return r.error();
  if (auto r = check(revoking ? stmt.bind_int64(2, now_millis()) : stmt.bind_null(2)); !r) return r.error();
  if (auto r = check(bind_optional_text(stmt, 3, reason)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, device_id)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::optional<EntryRow>> ServerStore::find_entry(std::string_view space_id,
                                                        const FileId& file_id) {
  auto statement = Statement::prepare(
      db_,
      "SELECT file_id,path,path_key,current_revision,content_hash,size,mtime_ms,deleted,"
      "editor_device,updated_seq,updated_at FROM entries WHERE space_id=? AND file_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, file_id.value)); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<EntryRow>{};
  }

  EntryRow entry;
  entry.file_id = FileId{stmt.column_text(0)};
  entry.path = stmt.column_text(1);
  entry.path_key = stmt.column_text(2);
  entry.revision = Revision{stmt.column_int64(3)};
  entry.content_hash = read_optional_hash(stmt, 4);
  entry.size = stmt.column_int64(5);
  entry.mtime_ms = stmt.column_int64(6);
  entry.deleted = stmt.column_int64(7) != 0;
  entry.editor_device = stmt.column_text(8);
  entry.updated_seq = Seq{stmt.column_int64(9)};
  entry.updated_at = stmt.column_int64(10);
  return std::optional<EntryRow>{std::move(entry)};
}

Result<void> ServerStore::upsert_entry(std::string_view space_id, const EntryRow& entry) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO entries(space_id,file_id,path,path_key,current_revision,content_hash,size,"
      "mtime_ms,deleted,editor_device,updated_seq,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(space_id,file_id) DO UPDATE SET path=excluded.path,"
      "path_key=excluded.path_key,current_revision=excluded.current_revision,"
      "content_hash=excluded.content_hash,size=excluded.size,mtime_ms=excluded.mtime_ms,"
      "deleted=excluded.deleted,editor_device=excluded.editor_device,"
      "updated_seq=excluded.updated_seq,updated_at=excluded.updated_at;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, entry.file_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_text(3, entry.path)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, entry.path_key)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(5, entry.revision.value)); !r) return r.error();
  if (auto r = check(bind_optional_hash(stmt, 6, entry.content_hash)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(7, entry.size)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(8, entry.mtime_ms)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(9, entry.deleted ? 1 : 0)); !r) return r.error();
  if (auto r = check(stmt.bind_text(10, entry.editor_device)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(11, entry.updated_seq.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(12, entry.updated_at)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<void> ServerStore::insert_version(const VersionRecord& version) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO versions(space_id,file_id,revision,parent_revision,op,path,content_hash,"
      "size,mtime_ms,device_id,op_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, version.space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, version.file_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(3, version.revision.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(4, version.parent_revision.value)); !r) return r.error();
  if (auto r = check(stmt.bind_text(5, to_string(version.op))); !r) return r.error();
  if (auto r = check(stmt.bind_text(6, version.path)); !r) return r.error();
  if (auto r = check(bind_optional_hash(stmt, 7, version.content_hash)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(8, version.size)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(9, version.mtime_ms)); !r) return r.error();
  if (auto r = check(stmt.bind_text(10, version.device_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(11, version.op_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(12, version.created_at)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

Result<std::optional<VersionRecord>> ServerStore::find_version(std::string_view space_id,
                                                               const FileId& file_id,
                                                               Revision revision) {
  auto statement = Statement::prepare(
      db_,
      "SELECT space_id,file_id,revision,parent_revision,op,path,content_hash,size,mtime_ms,"
      "device_id,op_id,created_at FROM versions WHERE space_id=? AND file_id=? AND revision=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, file_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(3, revision.value)); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<VersionRecord>{};
  }

  VersionRecord version;
  version.space_id = stmt.column_text(0);
  version.file_id = FileId{stmt.column_text(1)};
  version.revision = Revision{stmt.column_int64(2)};
  version.parent_revision = Revision{stmt.column_int64(3)};
  const auto op = op_kind_from_string(stmt.column_text(4));
  if (!op.has_value()) {
    return make_error(ErrorCode::DatabaseError, "invalid op value in versions table");
  }
  version.op = *op;
  version.path = stmt.column_text(5);
  version.content_hash = read_optional_hash(stmt, 6);
  version.size = stmt.column_int64(7);
  version.mtime_ms = stmt.column_int64(8);
  version.device_id = stmt.column_text(9);
  version.op_id = OpId{stmt.column_text(10)};
  version.created_at = stmt.column_int64(11);
  return std::optional<VersionRecord>{std::move(version)};
}

Result<Seq> ServerStore::append_change(const ChangeRecord& change) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO change_log(space_id,file_id,revision,op,path,content_hash,size,deleted,"
      "conflict,device_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, change.space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, change.file_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(3, change.revision.value)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, to_string(change.op))); !r) return r.error();
  if (auto r = check(stmt.bind_text(5, change.path)); !r) return r.error();
  if (auto r = check(bind_optional_hash(stmt, 6, change.content_hash)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(7, change.size)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(8, change.deleted ? 1 : 0)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(9, change.conflict ? 1 : 0)); !r) return r.error();
  if (auto r = check(stmt.bind_text(10, change.device_id)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(11, change.created_at)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  auto rowid = db_.last_insert_rowid();
  if (!rowid) {
    return rowid.error();
  }
  return Seq{rowid.value()};
}

Result<std::vector<ChangeRecord>> ServerStore::list_changes(std::string_view space_id, Seq since,
                                                            int limit) {
  auto statement = Statement::prepare(
      db_,
      "SELECT seq,space_id,file_id,revision,op,path,content_hash,size,deleted,conflict,"
      "device_id,created_at FROM change_log WHERE space_id=? AND seq>? ORDER BY seq ASC LIMIT ?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(2, since.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(3, limit)); !r) return r.error();

  std::vector<ChangeRecord> changes;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      break;
    }
    ChangeRecord change;
    change.seq = Seq{stmt.column_int64(0)};
    change.space_id = stmt.column_text(1);
    change.file_id = FileId{stmt.column_text(2)};
    change.revision = Revision{stmt.column_int64(3)};
    const auto op = op_kind_from_string(stmt.column_text(4));
    if (!op.has_value()) {
      return make_error(ErrorCode::DatabaseError, "invalid op value in change_log");
    }
    change.op = *op;
    change.path = stmt.column_text(5);
    change.content_hash = read_optional_hash(stmt, 6);
    change.size = stmt.column_int64(7);
    change.deleted = stmt.column_int64(8) != 0;
    change.conflict = stmt.column_int64(9) != 0;
    change.device_id = stmt.column_text(10);
    change.created_at = stmt.column_int64(11);
    changes.push_back(std::move(change));
  }
  return changes;
}

Result<std::optional<OperationRecord>> ServerStore::find_operation(const OpId& op_id) {
  auto statement = Statement::prepare(
      db_,
      "SELECT op_id,space_id,device_id,request_hash,response_code,response_body,seq,created_at "
      "FROM operations WHERE op_id=?;");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, op_id.value)); !r) return r.error();

  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::optional<OperationRecord>{};
  }

  OperationRecord operation;
  operation.op_id = OpId{stmt.column_text(0)};
  operation.space_id = stmt.column_text(1);
  operation.device_id = stmt.column_text(2);
  operation.request_hash = stmt.column_text(3);
  operation.response_code = static_cast<int>(stmt.column_int64(4));
  operation.response_body = stmt.column_text(5);
  operation.seq = Seq{stmt.column_int64(6)};
  operation.created_at = stmt.column_int64(7);
  return std::optional<OperationRecord>{std::move(operation)};
}

Result<void> ServerStore::insert_operation(const OperationRecord& operation) {
  auto statement = Statement::prepare(
      db_,
      "INSERT INTO operations(op_id,space_id,device_id,request_hash,response_code,"
      "response_body,seq,created_at) VALUES(?,?,?,?,?,?,?,?);");
  if (!statement) {
    return statement.error();
  }
  auto& stmt = statement.value();
  if (auto r = check(stmt.bind_text(1, operation.op_id.value)); !r) return r.error();
  if (auto r = check(stmt.bind_text(2, operation.space_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(3, operation.device_id)); !r) return r.error();
  if (auto r = check(stmt.bind_text(4, operation.request_hash)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(5, operation.response_code)); !r) return r.error();
  if (auto r = check(stmt.bind_text(6, operation.response_body)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(7, operation.seq.value)); !r) return r.error();
  if (auto r = check(stmt.bind_int64(8, operation.created_at)); !r) return r.error();
  auto step = stmt.step();
  if (!step) {
    return step.error();
  }
  return Result<void>{};
}

}  // namespace syncbox
