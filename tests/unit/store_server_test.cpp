#include <gtest/gtest.h>

#include <syncbox/core/uuid.hpp>
#include <syncbox/store/server_store.hpp>

#include <vector>

#include "test_support.hpp"

using syncbox::test::scalar_int;
using syncbox::test::TempDirectory;

namespace {

syncbox::EntryRow make_entry(const std::string& file_id, const std::string& path,
                             const std::string& path_key, std::int64_t revision,
                             std::int64_t seq, bool deleted = false) {
  syncbox::EntryRow entry;
  entry.file_id = syncbox::FileId{file_id};
  entry.path = path;
  entry.path_key = path_key;
  entry.revision = syncbox::Revision{revision};
  entry.content_hash = syncbox::ContentHash::from_hex(std::string(64, 'a'));
  entry.size = 3;
  entry.mtime_ms = 1;
  entry.deleted = deleted;
  entry.editor_device = "device-1";
  entry.updated_seq = syncbox::Seq{seq};
  entry.updated_at = 1;
  return entry;
}

syncbox::ChangeRecord make_change(const std::string& space_id, const std::string& file_id,
                                  std::int64_t revision) {
  syncbox::ChangeRecord change;
  change.space_id = space_id;
  change.file_id = syncbox::FileId{file_id};
  change.revision = syncbox::Revision{revision};
  change.op = syncbox::OpKind::Update;
  change.path = "a.txt";
  change.content_hash = syncbox::ContentHash::from_hex(std::string(64, 'b'));
  change.size = 3;
  change.deleted = false;
  change.conflict = false;
  change.device_id = "device-1";
  change.created_at = 1;
  return change;
}

}  // namespace

TEST(ServerStoreTest, SpaceAndDeviceRoundTrip) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  syncbox::SpaceRecord space;
  space.space_id = space_id;
  space.name = "Personal";
  space.created_at = 1;
  ASSERT_TRUE(store.value().insert_space(space));

  auto found_space = store.value().find_space(space_id);
  ASSERT_TRUE(found_space);
  ASSERT_TRUE(found_space.value().has_value());
  EXPECT_EQ(found_space.value()->name, "Personal");

  syncbox::DeviceRecord device;
  device.device_id = syncbox::generate_uuid_v7();
  device.space_id = space_id;
  device.display_name = "nix-laptop";
  device.platform = "linux";
  device.token_hash = std::vector<std::uint8_t>(32, 0x42);
  device.created_at = 1;
  ASSERT_TRUE(store.value().insert_device(device));

  auto found_device = store.value().find_device_by_token_hash(device.token_hash);
  ASSERT_TRUE(found_device);
  ASSERT_TRUE(found_device.value().has_value());
  EXPECT_EQ(found_device.value()->device_id, device.device_id);
  EXPECT_EQ(found_device.value()->status, "active");
}

TEST(ServerStoreTest, LivePathKeyIsUniqueAndTombstoneReleasesIt) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  syncbox::SpaceRecord space{space_id, "Space", 1, "active"};
  ASSERT_TRUE(store.value().insert_space(space));

  const std::string file_one = syncbox::generate_uuid_v7();
  const std::string file_two = syncbox::generate_uuid_v7();

  ASSERT_TRUE(store.value().upsert_entry(space_id, make_entry(file_one, "A.txt", "a.txt", 1, 1)));

  // Same path_key under a different file_id violates ux_entries_live_path.
  auto collision = store.value().upsert_entry(space_id, make_entry(file_two, "a.txt", "a.txt", 1, 2));
  EXPECT_FALSE(collision);

  // Marking the first entry deleted frees the path_key for the second.
  ASSERT_TRUE(store.value().upsert_entry(space_id, make_entry(file_one, "A.txt", "a.txt", 2, 3, true)));
  ASSERT_TRUE(store.value().upsert_entry(space_id, make_entry(file_two, "a.txt", "a.txt", 1, 4)));

  auto entry = store.value().find_entry(space_id, syncbox::FileId{file_two});
  ASSERT_TRUE(entry);
  ASSERT_TRUE(entry.value().has_value());
  EXPECT_FALSE(entry.value()->deleted);
  EXPECT_EQ(entry.value()->path_key, "a.txt");
}

TEST(ServerStoreTest, VersionRequiresExistingEntry) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  ASSERT_TRUE(store.value().insert_space({space_id, "Space", 1, "active"}));

  const std::string file_id = syncbox::generate_uuid_v7();
  syncbox::VersionRecord version;
  version.space_id = space_id;
  version.file_id = syncbox::FileId{file_id};
  version.revision = syncbox::Revision{1};
  version.parent_revision = syncbox::Revision{0};
  version.op = syncbox::OpKind::Create;
  version.path = "a.txt";
  version.content_hash = syncbox::ContentHash::from_hex(std::string(64, 'c'));
  version.size = 3;
  version.device_id = "device-1";
  version.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
  version.created_at = 1;

  EXPECT_FALSE(store.value().insert_version(version));

  ASSERT_TRUE(store.value().upsert_entry(space_id, make_entry(file_id, "a.txt", "a.txt", 1, 1)));
  ASSERT_TRUE(store.value().insert_version(version));

  auto found = store.value().find_version(space_id, syncbox::FileId{file_id}, syncbox::Revision{1});
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value().has_value());
  EXPECT_EQ(found.value()->path, "a.txt");
  EXPECT_EQ(found.value()->op, syncbox::OpKind::Create);
}

TEST(ServerStoreTest, ChangeLogSeqIsMonotonic) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string space_id = syncbox::generate_uuid_v7();
  ASSERT_TRUE(store.value().insert_space({space_id, "Space", 1, "active"}));
  const std::string file_id = syncbox::generate_uuid_v7();

  auto first = store.value().append_change(make_change(space_id, file_id, 1));
  auto second = store.value().append_change(make_change(space_id, file_id, 2));
  auto third = store.value().append_change(make_change(space_id, file_id, 3));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  EXPECT_LT(first.value().value, second.value().value);
  EXPECT_LT(second.value().value, third.value().value);

  auto changes = store.value().list_changes(space_id, first.value(), 10);
  ASSERT_TRUE(changes);
  ASSERT_EQ(changes.value().size(), 2U);
  EXPECT_EQ(changes.value().front().revision.value, 2);
}

TEST(ServerStoreTest, OperationIdIsUnique) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  syncbox::OperationRecord operation;
  operation.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
  operation.space_id = syncbox::generate_uuid_v7();
  operation.device_id = "device-1";
  operation.request_hash = std::string(64, 'd');
  operation.response_code = 200;
  operation.response_body = R"({"revision":1})";
  operation.seq = syncbox::Seq{1};
  operation.created_at = 1;

  ASSERT_TRUE(store.value().insert_operation(operation));
  EXPECT_FALSE(store.value().insert_operation(operation));

  auto found = store.value().find_operation(operation.op_id);
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value().has_value());
  EXPECT_EQ(found.value()->response_body, R"({"revision":1})");
}
