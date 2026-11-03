#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/server/blob_store.hpp>
#include <syncbox/store/server_store.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace syncbox::server {

struct FsckReport {
  bool ok = true;
  std::vector<std::string> problems;
  std::int64_t entries = 0;
  std::int64_t versions = 0;
  std::int64_t changes = 0;
  std::int64_t blobs = 0;
};

// Runs SQLite integrity/FK checks and verifies that live entries reference
// complete blobs. Read-only.
[[nodiscard]] Result<FsckReport> fsck(ServerStore& store);

// Deletes idempotency records older than the retention window. Returns the
// number of removed rows.
[[nodiscard]] Result<int> gc_operations(ServerStore& store, std::int64_t now_ms,
                                        int retention_days);

// Removes blobs with ref_count == 0 created more than `older_than_hours` ago.
[[nodiscard]] Result<int> gc_blobs(ServerStore& store, BlobStore& blobs,
                                   std::int64_t now_ms, int older_than_hours);

// Consistent online backup using SQLite's backup API.
[[nodiscard]] Result<void> backup_database(const std::string& source_path,
                                           const std::string& destination_path);

}  // namespace syncbox::server
