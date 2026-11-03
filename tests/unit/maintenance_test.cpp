#include <gtest/gtest.h>

#include <syncbox/common/time.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/server/blob_store.hpp>
#include <syncbox/server/maintenance.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <fstream>
#include <string>

#include "test_support.hpp"

using syncbox::test::TempDirectory;

namespace {

syncbox::OperationRecord make_operation(const std::string& space_id,
                                        std::int64_t created_at) {
  syncbox::OperationRecord operation;
  operation.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
  operation.space_id = space_id;
  operation.device_id = "device-1";
  operation.request_hash = std::string(64, 'a');
  operation.response_code = 200;
  operation.response_body = "{}";
  operation.seq = syncbox::Seq{1};
  operation.created_at = created_at;
  return operation;
}

}  // namespace

TEST(MaintenanceTest, FsckAcceptsFreshDatabase) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  auto report = syncbox::server::fsck(store.value());
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_TRUE(report.value().ok);
  EXPECT_TRUE(report.value().problems.empty());
  EXPECT_EQ(report.value().entries, 0);
  EXPECT_EQ(report.value().blobs, 0);
}

TEST(MaintenanceTest, FsckDetectsEntryWithoutCompleteBlob) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  ASSERT_TRUE(store.value().insert_space({space_id, "space", syncbox::now_millis(), "active"}));

  syncbox::EntryRow entry;
  entry.file_id = syncbox::FileId{syncbox::generate_uuid_v7()};
  entry.path = "a.txt";
  entry.path_key = "a.txt";
  entry.revision = syncbox::Revision{1};
  entry.content_hash = syncbox::ContentHash::from_hex(std::string(64, 'b'));
  entry.size = 3;
  entry.editor_device = "device-1";
  entry.updated_seq = syncbox::Seq{1};
  entry.updated_at = 1;
  ASSERT_TRUE(store.value().upsert_entry(space_id, entry));

  auto report = syncbox::server::fsck(store.value());
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_FALSE(report.value().ok);
  EXPECT_FALSE(report.value().problems.empty());
}

TEST(MaintenanceTest, GcRemovesOnlyExpiredOperations) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  const std::int64_t now = syncbox::now_millis();
  ASSERT_TRUE(store.value().insert_operation(
      make_operation(space_id, now - 10LL * 24 * 3600 * 1000)));
  ASSERT_TRUE(store.value().insert_operation(make_operation(space_id, now)));

  auto removed = syncbox::server::gc_operations(store.value(), now, 7);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(removed.value(), 1);

  auto remaining = syncbox::test::scalar_int(
      store.value().db(), "SELECT COUNT(*) FROM operations;");
  EXPECT_EQ(remaining, 1);
}

TEST(MaintenanceTest, GcRemovesOnlyUnreferencedOldBlobs) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  syncbox::server::BlobStore blobs(store.value(), directory.file("blobs"));
  ASSERT_TRUE(blobs.prepare());

  const std::int64_t now = syncbox::now_millis();
  auto make_blob = [&](const std::string& content, std::int64_t created_at,
                       bool referenced) {
    auto hash = syncbox::sha256(content);
    EXPECT_TRUE(hash);
    const std::string hex = hash.value().to_hex();

    syncbox::BlobRecord blob;
    blob.content_hash = hex;
    blob.size = static_cast<std::int64_t>(content.size());
    blob.chunk_size = static_cast<std::int64_t>(content.size());
    blob.chunk_count = 1;
    blob.state = "uploading";
    blob.created_at = created_at;
    EXPECT_TRUE(store.value().upsert_uploading_blob(blob));
    EXPECT_TRUE(store.value().mark_blob_complete(hex, created_at));

    const auto directory_path = blobs.root() / hex;
    std::filesystem::create_directories(directory_path);
    std::ofstream stream(directory_path / "0.chunk", std::ios::binary);
    stream << content;
    stream.close();

    if (referenced) {
      EXPECT_TRUE(store.value().increment_blob_ref(hex));
    }
    return hex;
  };

  const std::string old_hash = make_blob("old", now - 48LL * 3600 * 1000, false);
  const std::string young_hash = make_blob("young", now - 3600 * 1000, false);
  const std::string referenced_hash = make_blob("ref", now - 48LL * 3600 * 1000, true);

  auto removed = syncbox::server::gc_blobs(store.value(), blobs, now, 24);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(removed.value(), 1);

  EXPECT_FALSE(std::filesystem::exists(blobs.root() / old_hash));
  auto old_row = store.value().find_blob(old_hash);
  ASSERT_TRUE(old_row);
  EXPECT_FALSE(old_row.value().has_value());

  EXPECT_TRUE(std::filesystem::exists(blobs.root() / young_hash));
  EXPECT_TRUE(std::filesystem::exists(blobs.root() / referenced_hash));
}

TEST(MaintenanceTest, BackupProducesUsableDatabase) {
  TempDirectory directory;
  const auto source_path = directory.file("server.db");
  {
    auto store = syncbox::ServerStore::open(source_path.string());
    ASSERT_TRUE(store);
    ASSERT_TRUE(store.value().migrate());
    ASSERT_TRUE(store.value().insert_space(
        {syncbox::generate_uuid_v7(), "backed-up", syncbox::now_millis(), "active"}));
  }

  const auto backup_path = directory.file("backup.db");
  auto result = syncbox::server::backup_database(source_path.string(),
                                                 backup_path.string());
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_TRUE(std::filesystem::exists(backup_path));

  auto backup = syncbox::ServerStore::open(backup_path.string());
  ASSERT_TRUE(backup);
  auto count = syncbox::test::scalar_int(backup.value().db(),
                                         "SELECT COUNT(*) FROM spaces;");
  EXPECT_EQ(count, 1);
}
