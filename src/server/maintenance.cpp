#include <syncbox/server/maintenance.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>

namespace syncbox::server {

namespace {

[[nodiscard]] Result<std::int64_t> scalar(ServerStore& store, std::string_view sql) {
  auto statement = Statement::prepare(store.db(), sql);
  if (!statement) {
    return statement.error();
  }
  auto step = statement.value().step();
  if (!step) {
    return step.error();
  }
  if (!step.value()) {
    return std::int64_t{0};
  }
  return statement.value().column_int64(0);
}

}  // namespace

Result<FsckReport> fsck(ServerStore& store) {
  FsckReport report;
  auto& db = store.db();

  {
    auto statement = Statement::prepare(db, "PRAGMA integrity_check;");
    if (!statement) {
      return statement.error();
    }
    auto step = statement.value().step();
    if (!step) {
      return step.error();
    }
    if (!step.value() || statement.value().column_text(0) != "ok") {
      report.ok = false;
      report.problems.push_back("integrity_check failed");
    }
  }

  {
    auto statement = Statement::prepare(db, "PRAGMA foreign_key_check;");
    if (!statement) {
      return statement.error();
    }
    while (true) {
      auto step = statement.value().step();
      if (!step) {
        return step.error();
      }
      if (!step.value()) {
        break;
      }
      report.ok = false;
      report.problems.push_back("foreign key violation in " +
                                statement.value().column_text(0));
    }
  }

  auto entries = scalar(store, "SELECT COUNT(*) FROM entries;");
  auto versions = scalar(store, "SELECT COUNT(*) FROM versions;");
  auto changes = scalar(store, "SELECT COUNT(*) FROM change_log;");
  auto blobs = scalar(store, "SELECT COUNT(*) FROM blobs;");
  if (!entries || !versions || !changes || !blobs) {
    return make_error(ErrorCode::DatabaseError, "cannot count tables");
  }
  report.entries = entries.value();
  report.versions = versions.value();
  report.changes = changes.value();
  report.blobs = blobs.value();

  auto missing = scalar(
      store,
      "SELECT COUNT(*) FROM entries e LEFT JOIN blobs b "
      "ON e.content_hash = b.content_hash "
      "WHERE e.deleted = 0 AND e.content_hash IS NOT NULL "
      "AND (b.content_hash IS NULL OR b.state != 'complete');");
  if (!missing) {
    return missing.error();
  }
  if (missing.value() > 0) {
    report.ok = false;
    report.problems.push_back(std::to_string(missing.value()) +
                              " live entries reference a missing/incomplete blob");
  }

  return report;
}

Result<int> gc_operations(ServerStore& store, std::int64_t now_ms, int retention_days) {
  if (retention_days < 0) {
    return make_error(ErrorCode::BadRequest, "retention_days must be >= 0");
  }
  const std::int64_t cutoff =
      now_ms - static_cast<std::int64_t>(retention_days) * 24 * 3600 * 1000;

  auto statement =
      Statement::prepare(store.db(), "DELETE FROM operations WHERE created_at < ?;");
  if (!statement) {
    return statement.error();
  }
  if (auto bound = statement.value().bind_int64(1, cutoff); !bound) {
    return bound.error();
  }
  auto step = statement.value().step();
  if (!step) {
    return step.error();
  }
  return store.db().changes();
}

Result<int> gc_blobs(ServerStore& store, BlobStore& blobs, std::int64_t now_ms,
                     int older_than_hours) {
  if (older_than_hours < 0) {
    return make_error(ErrorCode::BadRequest, "older_than_hours must be >= 0");
  }
  const std::int64_t cutoff =
      now_ms - static_cast<std::int64_t>(older_than_hours) * 3600 * 1000;

  int removed = 0;
  while (true) {
    auto candidates = store.list_gc_blob_candidates(cutoff, 1000);
    if (!candidates) {
      return candidates.error();
    }
    if (candidates.value().empty()) {
      break;
    }
    for (const auto& hash : candidates.value()) {
      auto files = blobs.remove_files(hash);
      if (!files) {
        return files.error();
      }
      auto row = store.delete_blob(hash);
      if (!row) {
        return row.error();
      }
      ++removed;
    }
    if (candidates.value().size() < 1000) {
      break;
    }
  }
  return removed;
}

Result<int> gc_change_log(ServerStore& store, std::int64_t now_ms, int retention_days) {
  if (retention_days < 0) {
    return make_error(ErrorCode::BadRequest, "retention_days must be >= 0");
  }
  const std::int64_t cutoff =
      now_ms - static_cast<std::int64_t>(retention_days) * 24 * 3600 * 1000;

  std::int64_t active_devices = 0;
  std::int64_t oldest_ack = 0;
  {
    auto statement = Statement::prepare(
        store.db(),
        "SELECT COUNT(*), COALESCE(MIN(last_ack_seq), 0) FROM devices "
        "WHERE status='active';");
    if (!statement) {
      return statement.error();
    }
    auto step = statement.value().step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      return 0;
    }
    active_devices = statement.value().column_int64(0);
    oldest_ack = statement.value().column_int64(1);
  }

  std::int64_t horizon = 0;
  {
    auto statement = Statement::prepare(
        store.db(),
        "SELECT COALESCE(MAX(seq), 0) FROM change_log WHERE created_at < ?;");
    if (!statement) {
      return statement.error();
    }
    if (auto bound = statement.value().bind_int64(1, cutoff); !bound) {
      return bound.error();
    }
    auto step = statement.value().step();
    if (!step) {
      return step.error();
    }
    if (!step.value()) {
      return 0;
    }
    horizon = statement.value().column_int64(0);
  }

  std::int64_t watermark = active_devices == 0 ? horizon : std::min(oldest_ack, horizon);

  auto existing = store.get_meta("min_retained_seq");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().has_value()) {
    try {
      watermark = std::max(
          watermark, static_cast<std::int64_t>(std::stoll(*existing.value())));
    } catch (const std::exception&) {
      return make_error(ErrorCode::DatabaseError,
                        "min_retained_seq is not a valid integer");
    }
  }

  if (watermark <= 0) {
    return 0;
  }

  auto statement =
      Statement::prepare(store.db(), "DELETE FROM change_log WHERE seq < ?;");
  if (!statement) {
    return statement.error();
  }
  if (auto bound = statement.value().bind_int64(1, watermark); !bound) {
    return bound.error();
  }
  auto step = statement.value().step();
  if (!step) {
    return step.error();
  }

  auto stored = store.set_meta("min_retained_seq", std::to_string(watermark));
  if (!stored) {
    return stored.error();
  }
  return store.db().changes();
}

Result<void> backup_database(const std::string& source_path,
                             const std::string& destination_path) {
  sqlite3* source = nullptr;
  sqlite3* destination = nullptr;

  if (sqlite3_open_v2(source_path.c_str(), &source, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    const std::string message = source != nullptr ? sqlite3_errmsg(source) : "open failed";
    if (source != nullptr) {
      sqlite3_close(source);
    }
    return make_error(ErrorCode::DatabaseError, "cannot open source database: " + message);
  }
  if (sqlite3_open_v2(destination_path.c_str(), &destination,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string message =
        destination != nullptr ? sqlite3_errmsg(destination) : "open failed";
    sqlite3_close(source);
    if (destination != nullptr) {
      sqlite3_close(destination);
    }
    return make_error(ErrorCode::DatabaseError,
                      "cannot open backup destination: " + message);
  }

  sqlite3_backup* backup =
      sqlite3_backup_init(destination, "main", source, "main");
  if (backup == nullptr) {
    const std::string message = sqlite3_errmsg(destination);
    sqlite3_close(destination);
    sqlite3_close(source);
    return make_error(ErrorCode::DatabaseError, "backup init failed: " + message);
  }

  const int step = sqlite3_backup_step(backup, -1);
  const int finish = sqlite3_backup_finish(backup);
  sqlite3_close(destination);
  sqlite3_close(source);

  if (step != SQLITE_DONE || finish != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "database backup failed");
  }
  return Result<void>{};
}

}  // namespace syncbox::server
