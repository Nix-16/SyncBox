#include <gtest/gtest.h>

#include <syncbox/common/time.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/server/maintenance.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

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
