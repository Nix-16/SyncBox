#include <syncbox/store/migrations.hpp>

#include <chrono>
#include <string>

namespace syncbox {

namespace {

constexpr char kServerSchemaV1[] = R"sql(
CREATE TABLE schema_migrations (
  version     INTEGER PRIMARY KEY,
  applied_at  INTEGER NOT NULL
);

CREATE TABLE server_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE spaces (
  space_id    TEXT PRIMARY KEY,
  name        TEXT NOT NULL,
  created_at  INTEGER NOT NULL,
  state       TEXT NOT NULL DEFAULT 'active'
              CHECK (state IN ('active','archived'))
);

CREATE TABLE devices (
  device_id      TEXT PRIMARY KEY,
  space_id       TEXT NOT NULL REFERENCES spaces(space_id),
  display_name   TEXT NOT NULL,
  platform       TEXT NOT NULL CHECK (platform IN ('linux','windows','macos')),
  token_hash     BLOB NOT NULL UNIQUE,
  status         TEXT NOT NULL DEFAULT 'active'
                 CHECK (status IN ('active','revoked')),
  app_version    TEXT,
  created_at     INTEGER NOT NULL,
  last_seen_at   INTEGER,
  last_ack_seq   INTEGER NOT NULL DEFAULT 0,
  revoked_at     INTEGER,
  revoked_reason TEXT
);
CREATE INDEX ix_devices_space ON devices(space_id, status);
CREATE INDEX ix_devices_ack   ON devices(space_id, last_ack_seq);

CREATE TABLE entries (
  space_id          TEXT NOT NULL REFERENCES spaces(space_id),
  file_id           TEXT NOT NULL,
  path              TEXT NOT NULL,
  path_key          TEXT NOT NULL,
  current_revision  INTEGER NOT NULL CHECK (current_revision >= 1),
  content_hash      TEXT,
  size              INTEGER,
  mtime_ms          INTEGER,
  deleted           INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0,1)),
  editor_device     TEXT,
  updated_seq       INTEGER NOT NULL,
  updated_at        INTEGER NOT NULL,
  PRIMARY KEY (space_id, file_id)
);
CREATE UNIQUE INDEX ux_entries_live_path
  ON entries(space_id, path_key) WHERE deleted = 0;
CREATE INDEX ix_entries_seq  ON entries(space_id, updated_seq);
CREATE INDEX ix_entries_path ON entries(space_id, path);

CREATE TABLE versions (
  space_id        TEXT NOT NULL,
  file_id         TEXT NOT NULL,
  revision        INTEGER NOT NULL CHECK (revision >= 1),
  parent_revision INTEGER NOT NULL CHECK (parent_revision >= 0),
  op              TEXT NOT NULL
                  CHECK (op IN ('create','update','delete','rename','restore')),
  path            TEXT NOT NULL,
  content_hash    TEXT,
  size            INTEGER,
  mtime_ms        INTEGER,
  device_id       TEXT NOT NULL,
  op_id           TEXT NOT NULL,
  created_at      INTEGER NOT NULL,
  PRIMARY KEY (space_id, file_id, revision),
  FOREIGN KEY (space_id, file_id) REFERENCES entries(space_id, file_id)
);
CREATE INDEX ix_versions_op     ON versions(space_id, op_id);
CREATE INDEX ix_versions_device ON versions(space_id, device_id, created_at);

CREATE TABLE change_log (
  seq          INTEGER PRIMARY KEY AUTOINCREMENT,
  space_id     TEXT NOT NULL,
  file_id      TEXT NOT NULL,
  revision     INTEGER NOT NULL,
  op           TEXT NOT NULL,
  path         TEXT NOT NULL,
  content_hash TEXT,
  size         INTEGER,
  deleted      INTEGER NOT NULL CHECK (deleted IN (0,1)),
  conflict     INTEGER NOT NULL DEFAULT 0 CHECK (conflict IN (0,1)),
  device_id    TEXT NOT NULL,
  created_at   INTEGER NOT NULL
);
CREATE INDEX ix_changelog_space_seq ON change_log(space_id, seq);
CREATE INDEX ix_changelog_file      ON change_log(space_id, file_id, seq);

CREATE TABLE operations (
  op_id         TEXT PRIMARY KEY,
  space_id      TEXT NOT NULL,
  device_id     TEXT NOT NULL,
  request_hash  TEXT NOT NULL,
  response_code INTEGER NOT NULL,
  response_body TEXT NOT NULL,
  seq           INTEGER NOT NULL,
  created_at    INTEGER NOT NULL
);
CREATE INDEX ix_operations_created ON operations(created_at);

CREATE TABLE blobs (
  content_hash TEXT PRIMARY KEY,
  size         INTEGER NOT NULL,
  chunk_size   INTEGER NOT NULL,
  chunk_count  INTEGER NOT NULL,
  state        TEXT NOT NULL CHECK (state IN ('uploading','complete')),
  ref_count    INTEGER NOT NULL DEFAULT 0,
  created_at   INTEGER NOT NULL,
  completed_at INTEGER
);
CREATE INDEX ix_blobs_state ON blobs(state, created_at);

CREATE TABLE blob_chunks (
  content_hash TEXT NOT NULL REFERENCES blobs(content_hash) ON DELETE CASCADE,
  chunk_index  INTEGER NOT NULL,
  chunk_hash   TEXT NOT NULL,
  size         INTEGER NOT NULL,
  received_at  INTEGER,
  PRIMARY KEY (content_hash, chunk_index)
);

CREATE TABLE enrollment_tokens (
  token_hash BLOB PRIMARY KEY,
  space_id   TEXT NOT NULL REFERENCES spaces(space_id),
  created_by TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  used_by    TEXT,
  used_at    INTEGER
);
CREATE INDEX ix_enroll_expiry ON enrollment_tokens(expires_at);
)sql";

constexpr char kClientSchemaV1[] = R"sql(
CREATE TABLE schema_migrations (
  version    INTEGER PRIMARY KEY,
  applied_at INTEGER NOT NULL
);

CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE local_entries (
  file_id          TEXT PRIMARY KEY,
  path             TEXT NOT NULL,
  path_key         TEXT NOT NULL,
  server_revision  INTEGER NOT NULL DEFAULT 0,
  content_hash     TEXT,
  size             INTEGER,
  mtime_ms         INTEGER,
  deleted          INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0,1)),
  dirty            INTEGER NOT NULL DEFAULT 0 CHECK (dirty IN (0,1)),
  fs_ino           INTEGER,
  fs_dev           INTEGER,
  applied_seq      INTEGER NOT NULL DEFAULT 0,
  conflict_of      TEXT,
  updated_at       INTEGER NOT NULL
);
CREATE INDEX ix_local_path_key ON local_entries(path_key);
CREATE INDEX ix_local_path     ON local_entries(path);

CREATE TABLE pending_ops (
  op_id          TEXT PRIMARY KEY,
  file_id        TEXT,
  kind           TEXT NOT NULL
                 CHECK (kind IN ('create','update','delete','rename','restore')),
  base_revision  INTEGER NOT NULL,
  path           TEXT NOT NULL,
  new_path       TEXT,
  content_hash   TEXT,
  size           INTEGER,
  staged_path    TEXT,
  client_seq     INTEGER NOT NULL,
  state          TEXT NOT NULL
                 CHECK (state IN ('staged','uploading','committing','retry',
                                  'conflict','done','dead')),
  attempts       INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER,
  last_error     TEXT,
  created_at     INTEGER NOT NULL,
  updated_at     INTEGER NOT NULL
);
CREATE INDEX ix_pending_state ON pending_ops(state, next_attempt_at);
CREATE INDEX ix_pending_file  ON pending_ops(file_id);
CREATE UNIQUE INDEX ux_pending_active_file
  ON pending_ops(file_id)
  WHERE file_id IS NOT NULL
    AND state IN ('staged','uploading','committing','retry');

CREATE TABLE apply_journal (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  file_id       TEXT NOT NULL,
  target_path   TEXT NOT NULL,
  temp_path     TEXT NOT NULL,
  expected_hash TEXT,
  expected_size INTEGER,
  state         TEXT NOT NULL
                CHECK (state IN ('downloading','staged','replacing','done','failed')),
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL
);
CREATE INDEX ix_apply_state ON apply_journal(state);
CREATE INDEX ix_apply_file  ON apply_journal(file_id);

CREATE TABLE conflicts (
  conflict_id TEXT PRIMARY KEY,
  file_id     TEXT NOT NULL,
  kind        TEXT NOT NULL
              CHECK (kind IN ('modify_modify','modify_delete','rename_conflict',
                              'path_collision','case_collision','hash_mismatch')),
  local_path  TEXT,
  remote_path TEXT,
  resolution  TEXT NOT NULL
              CHECK (resolution IN ('conflict_copy','remote_wins','restored','manual_pending')),
  detail      TEXT,
  created_at  INTEGER NOT NULL,
  resolved_at INTEGER
);
CREATE INDEX ix_conflicts_pending ON conflicts(resolved_at);

CREATE TABLE write_suppressions (
  path_key TEXT PRIMARY KEY,
  until_ms INTEGER NOT NULL,
  reason   TEXT NOT NULL
);

CREATE TABLE download_parts (
  content_hash TEXT NOT NULL,
  chunk_index  INTEGER NOT NULL,
  temp_path    TEXT,
  received     INTEGER NOT NULL DEFAULT 0 CHECK (received IN (0,1)),
  updated_at   INTEGER NOT NULL,
  PRIMARY KEY (content_hash, chunk_index)
);

CREATE TABLE rename_candidates (
  path_key     TEXT PRIMARY KEY,
  fs_ino       INTEGER,
  fs_dev       INTEGER,
  content_hash TEXT,
  seen_at      INTEGER NOT NULL
);
)sql";

[[nodiscard]] Result<void> apply_migration(Database& db, const char* ddl) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  auto transaction = Transaction::begin(db);
  if (!transaction) {
    return transaction.error();
  }

  auto ddl_result = db.execute(ddl);
  if (!ddl_result) {
    return ddl_result.error();
  }

  const std::string insert =
      "INSERT INTO schema_migrations(version, applied_at) VALUES (" +
      std::to_string(kSchemaVersion) + ", " + std::to_string(now) + ");";
  auto insert_result = db.execute(insert);
  if (!insert_result) {
    return insert_result.error();
  }

  auto version_result = db.set_user_version(kSchemaVersion);
  if (!version_result) {
    return version_result.error();
  }

  return transaction.value().commit();
}

}  // namespace

Result<void> migrate_server(Database& db) {
  const int current = db.user_version();
  if (current > kSchemaVersion) {
    return make_error(ErrorCode::DatabaseError,
                      "database schema is newer than this program supports");
  }
  if (current == kSchemaVersion) {
    return Result<void>{};
  }
  return apply_migration(db, kServerSchemaV1);
}

Result<void> migrate_client(Database& db) {
  const int current = db.user_version();
  if (current > kSchemaVersion) {
    return make_error(ErrorCode::DatabaseError,
                      "database schema is newer than this program supports");
  }
  if (current == kSchemaVersion) {
    return Result<void>{};
  }
  return apply_migration(db, kClientSchemaV1);
}

}  // namespace syncbox
