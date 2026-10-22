#pragma once

#include <syncbox/core/result.hpp>

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace syncbox {

enum class DatabaseMode {
  Server,
  Client,
};

struct DatabaseOptions {
  DatabaseMode mode = DatabaseMode::Client;
  bool create_if_missing = true;
};

// Thin RAII wrapper around sqlite3. All write access is expected to be
// serialized by the caller (single writer thread), as documented in
// ARCHITECTURE.md section 5.
class Database {
 public:
  static Result<Database> open(const std::string& path, DatabaseOptions options = {});

  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;

  [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

  [[nodiscard]] Result<void> execute(std::string_view sql);
  [[nodiscard]] Result<std::int64_t> last_insert_rowid() const;
  [[nodiscard]] Result<void> set_user_version(int version);
  [[nodiscard]] int user_version() const;

 private:
  explicit Database(sqlite3* db) noexcept : db_(db) {}
  sqlite3* db_ = nullptr;
};

class Statement {
 public:
  static Result<Statement> prepare(Database& db, std::string_view sql);

  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;

  [[nodiscard]] Result<void> bind_text(int index, std::string_view value);
  [[nodiscard]] Result<void> bind_int64(int index, std::int64_t value);
  [[nodiscard]] Result<void> bind_null(int index);
  [[nodiscard]] Result<void> bind_blob(int index, const void* data, int size);

  // true: one row available; false: statement finished.
  [[nodiscard]] Result<bool> step();
  [[nodiscard]] Result<void> reset();
  [[nodiscard]] Result<void> clear_bindings();

  [[nodiscard]] std::string column_text(int column) const;
  [[nodiscard]] std::int64_t column_int64(int column) const;
  [[nodiscard]] bool column_is_null(int column) const;

 private:
  explicit Statement(sqlite3_stmt* stmt) noexcept : stmt_(stmt) {}
  sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
 public:
  static Result<Transaction> begin(Database& db, bool immediate = true);

  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;

  [[nodiscard]] Result<void> commit();

 private:
  explicit Transaction(Database* db) noexcept : db_(db), active_(true) {}
  Database* db_ = nullptr;
  bool active_ = false;
};

}  // namespace syncbox
