#include <gtest/gtest.h>

#include <syncbox/core/uuid.hpp>
#include <syncbox/store/client_store.hpp>

#include <string>

#include "test_support.hpp"

using syncbox::test::scalar_int;
using syncbox::test::TempDirectory;

namespace {

syncbox::LocalEntryRow make_local_entry(const std::string& file_id, const std::string& path,
                                        const std::string& path_key) {
  syncbox::LocalEntryRow entry;
  entry.file_id = syncbox::FileId{file_id};
  entry.path = path;
  entry.path_key = path_key;
  entry.server_revision = syncbox::Revision{3};
  entry.content_hash = syncbox::ContentHash::from_hex(std::string(64, 'e'));
  entry.size = 10;
  entry.mtime_ms = 100;
  entry.deleted = false;
  entry.dirty = false;
  entry.fs_ino = 42;
  entry.fs_dev = 7;
  entry.applied_seq = syncbox::Seq{9};
  entry.updated_at = 100;
  return entry;
}

syncbox::PendingOpRow make_pending_op(const std::string& file_id, const std::string& state) {
  syncbox::PendingOpRow op;
  op.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
  op.file_id = syncbox::FileId{file_id};
  op.kind = syncbox::OpKind::Update;
  op.base_revision = syncbox::Revision{3};
  op.path = "a.txt";
  op.client_seq = syncbox::Seq{9};
  op.state = state;
  op.created_at = 1;
  op.updated_at = 1;
  return op;
}

}  // namespace

TEST(ClientStoreTest, CursorOnlyMovesForward) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  auto initial = store.value().cursor();
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial.value().value, 0);

  ASSERT_TRUE(store.value().set_cursor(syncbox::Seq{10}));
  ASSERT_TRUE(store.value().set_cursor(syncbox::Seq{5}));

  auto current = store.value().cursor();
  ASSERT_TRUE(current);
  EXPECT_EQ(current.value().value, 10);
}

TEST(ClientStoreTest, LocalEntryRoundTrip) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string file_id = syncbox::generate_uuid_v7();
  ASSERT_TRUE(store.value().upsert_local_entry(make_local_entry(file_id, "A.txt", "a.txt")));

  auto by_id = store.value().find_local_entry(syncbox::FileId{file_id});
  ASSERT_TRUE(by_id);
  ASSERT_TRUE(by_id.value().has_value());
  EXPECT_EQ(by_id.value()->path, "A.txt");
  EXPECT_EQ(by_id.value()->server_revision.value, 3);

  auto by_key = store.value().find_local_entry_by_path_key("a.txt");
  ASSERT_TRUE(by_key);
  ASSERT_TRUE(by_key.value().has_value());
  EXPECT_EQ(by_key.value()->file_id.value, file_id);

  auto all = store.value().list_local_entries(false);
  ASSERT_TRUE(all);
  EXPECT_EQ(all.value().size(), 1U);
}

TEST(ClientStoreTest, OnlyOneActiveOperationPerFile) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  const std::string file_id = syncbox::generate_uuid_v7();
  auto first = make_pending_op(file_id, "staged");
  ASSERT_TRUE(store.value().enqueue_pending_op(first));

  auto second = make_pending_op(file_id, "staged");
  EXPECT_FALSE(store.value().enqueue_pending_op(second));

  ASSERT_TRUE(store.value().update_pending_state(first.op_id, "done", std::nullopt, std::nullopt));
  auto third = make_pending_op(file_id, "staged");
  EXPECT_TRUE(store.value().enqueue_pending_op(third));
}

TEST(ClientStoreTest, PendingStateUpdatesAndListing) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  auto op = make_pending_op(syncbox::generate_uuid_v7(), "retry");
  ASSERT_TRUE(store.value().enqueue_pending_op(op));

  ASSERT_TRUE(store.value().update_pending_state(op.op_id, "dead", "too many attempts", 1234));

  auto retry_ops = store.value().list_pending_ops(std::string("retry"));
  ASSERT_TRUE(retry_ops);
  EXPECT_TRUE(retry_ops.value().empty());

  auto all = store.value().list_pending_ops();
  ASSERT_TRUE(all);
  ASSERT_EQ(all.value().size(), 1U);
  EXPECT_EQ(all.value().front().state, "dead");
  EXPECT_EQ(all.value().front().last_error, "too many attempts");
  EXPECT_EQ(all.value().front().next_attempt_at, 1234);

  ASSERT_TRUE(store.value().delete_pending_op(op.op_id));
  auto after_delete = store.value().list_pending_ops();
  ASSERT_TRUE(after_delete);
  EXPECT_TRUE(after_delete.value().empty());
}

TEST(ClientStoreTest, ConflictsListOnlyUnresolved) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  syncbox::ConflictRow conflict;
  conflict.conflict_id = syncbox::generate_uuid_v7();
  conflict.file_id = syncbox::FileId{syncbox::generate_uuid_v7()};
  conflict.kind = "modify_modify";
  conflict.local_path = "a.txt";
  conflict.remote_path = "a.txt";
  conflict.resolution = "conflict_copy";
  conflict.created_at = 10;
  ASSERT_TRUE(store.value().insert_conflict(conflict));

  auto unresolved = store.value().list_unresolved_conflicts();
  ASSERT_TRUE(unresolved);
  ASSERT_EQ(unresolved.value().size(), 1U);
  EXPECT_EQ(unresolved.value().front().kind, "modify_modify");

  auto& db = store.value().db();
  ASSERT_TRUE(db.execute("UPDATE conflicts SET resolved_at=20;"));
  auto after = store.value().list_unresolved_conflicts();
  ASSERT_TRUE(after);
  EXPECT_TRUE(after.value().empty());
  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM conflicts;"), 1);
}
