#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/store/database.hpp>

namespace syncbox {

inline constexpr int kSchemaVersion = 1;

// Creates the server schema (docs/DATABASE.md section 2.2) when the database
// is empty. Re-running on an up-to-date database is a no-op. Refuses to open a
// database created by a newer program version.
[[nodiscard]] Result<void> migrate_server(Database& db);

// Creates the client schema (docs/DATABASE.md section 3.1).
[[nodiscard]] Result<void> migrate_client(Database& db);

}  // namespace syncbox
