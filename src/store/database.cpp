#include <syncbox/store/database.hpp>

#include <utility>

namespace syncbox {

namespace {

[[nodiscard]] Error sqlite_error(sqlite3* db, std::string_view context) {
  std::string message(context);
  if (db != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(db);
  }
  return make_error(ErrorCode::DatabaseError, std::move(message));
}

void configure_connection(sqlite3* db, DatabaseMode mode) {
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
  if (mode == DatabaseMode::Server) {
    sqlite3_exec(db, "PRAGMA synchronous=FULL;", nullptr, nullptr, nullptr);
  } else {
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  }
}

}  // namespace

Result<Database> Database::open(const std::string& path, DatabaseOptions options) {
  int flags = SQLITE_OPEN_READWRITE;
  if (options.create_if_missing) {
    flags |= SQLITE_OPEN_CREATE;
  }

  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    const Error error = sqlite_error(db, "cannot open database " + path);
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return error;
  }

  configure_connection(db, options.mode);
  return Database(db);
}

Database::~Database() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

Database::Database(Database&& other) noexcept : db_(other.db_) {
  other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

Result<void> Database::execute(std::string_view sql) {
  if (db_ == nullptr) {
    return make_error(ErrorCode::DatabaseError, "database is not open");
  }
  char* error_message = nullptr;
  const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error_message);
  if (rc != SQLITE_OK) {
    std::string message = "sqlite3_exec failed";
    if (error_message != nullptr) {
      message += ": ";
      message += error_message;
      sqlite3_free(error_message);
    }
    return make_error(ErrorCode::DatabaseError, std::move(message));
  }
  return Result<void>{};
}

Result<std::int64_t> Database::last_insert_rowid() const {
  if (db_ == nullptr) {
    return make_error(ErrorCode::DatabaseError, "database is not open");
  }
  return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const {
  return db_ == nullptr ? 0 : sqlite3_changes(db_);
}

Result<void> Database::set_user_version(int version) {
  return execute("PRAGMA user_version=" + std::to_string(version) + ";");
}

int Database::user_version() const {
  if (db_ == nullptr) {
    return -1;
  }
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }
  int version = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return version;
}

Result<Statement> Statement::prepare(Database& db, std::string_view sql) {
  if (db.handle() == nullptr) {
    return make_error(ErrorCode::DatabaseError, "database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db.handle(), std::string(sql).c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return sqlite_error(db.handle(), "sqlite3_prepare_v2 failed");
  }
  return Statement(stmt);
}

Statement::~Statement() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
}

Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
    stmt_ = other.stmt_;
    other.stmt_ = nullptr;
  }
  return *this;
}

Result<void> Statement::bind_text(int index, std::string_view value) {
  const int rc = sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_bind_text failed");
  }
  return Result<void>{};
}

Result<void> Statement::bind_int64(int index, std::int64_t value) {
  const int rc = sqlite3_bind_int64(stmt_, index, value);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_bind_int64 failed");
  }
  return Result<void>{};
}

Result<void> Statement::bind_null(int index) {
  const int rc = sqlite3_bind_null(stmt_, index);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_bind_null failed");
  }
  return Result<void>{};
}

Result<void> Statement::bind_blob(int index, const void* data, int size) {
  const int rc = sqlite3_bind_blob(stmt_, index, data, size, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_bind_blob failed");
  }
  return Result<void>{};
}

Result<bool> Statement::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  return make_error(ErrorCode::DatabaseError,
                    std::string("sqlite3_step failed: ") + sqlite3_errstr(rc));
}

Result<void> Statement::reset() {
  const int rc = sqlite3_reset(stmt_);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_reset failed");
  }
  return Result<void>{};
}

Result<void> Statement::clear_bindings() {
  const int rc = sqlite3_clear_bindings(stmt_);
  if (rc != SQLITE_OK) {
    return make_error(ErrorCode::DatabaseError, "sqlite3_clear_bindings failed");
  }
  return Result<void>{};
}

std::string Statement::column_text(int column) const {
  const auto* text = sqlite3_column_text(stmt_, column);
  if (text == nullptr) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt_, column);
  return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

std::int64_t Statement::column_int64(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

bool Statement::column_is_null(int column) const {
  return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

std::vector<std::uint8_t> Statement::column_blob(int column) const {
  const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_, column));
  if (data == nullptr) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt_, column);
  return std::vector<std::uint8_t>(data, data + size);
}

Result<Transaction> Transaction::begin(Database& db, bool immediate) {
  const char* sql = immediate ? "BEGIN IMMEDIATE;" : "BEGIN;";
  auto result = db.execute(sql);
  if (!result) {
    return result.error();
  }
  return Transaction(&db);
}

Transaction::~Transaction() {
  if (active_ && db_ != nullptr) {
    (void)db_->execute("ROLLBACK;");
  }
}

Transaction::Transaction(Transaction&& other) noexcept : db_(other.db_), active_(other.active_) {
  other.db_ = nullptr;
  other.active_ = false;
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    if (active_ && db_ != nullptr) {
      (void)db_->execute("ROLLBACK;");
    }
    db_ = other.db_;
    active_ = other.active_;
    other.db_ = nullptr;
    other.active_ = false;
  }
  return *this;
}

Result<void> Transaction::commit() {
  if (!active_ || db_ == nullptr) {
    return make_error(ErrorCode::DatabaseError, "transaction is not active");
  }
  auto result = db_->execute("COMMIT;");
  if (!result) {
    return result.error();
  }
  active_ = false;
  return Result<void>{};
}

}  // namespace syncbox
