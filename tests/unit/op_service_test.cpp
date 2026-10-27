#include <gtest/gtest.h>

#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/server/op_service.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <optional>
#include <string>
#include <variant>

#include "test_support.hpp"

namespace {

using syncbox::server::CommitFailure;
using syncbox::server::CommitOutcome;
using syncbox::server::CommitSuccess;

[[nodiscard]] bool succeeded(const CommitOutcome& outcome) {
  return std::holds_alternative<CommitSuccess>(outcome);
}

[[nodiscard]] syncbox::ErrorCode failure_code(const CommitOutcome& outcome) {
  return std::get<CommitFailure>(outcome).code;
}

}  // namespace

class OpServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto store = syncbox::ServerStore::open(directory_.file("server.db").string());
    ASSERT_TRUE(store);
    ASSERT_TRUE(store.value().migrate());
    store_ = std::move(store.value());

    space_id_ = syncbox::generate_uuid_v7();
    syncbox::SpaceRecord space{space_id_, "Test", syncbox::now_millis(), "active"};
    ASSERT_TRUE(store_->insert_space(space));
    ops_ = std::make_unique<syncbox::server::OpService>(*store_);
  }

  syncbox::ContentHash make_blob(const std::string& content) {
    auto hash = syncbox::sha256(content);
    EXPECT_TRUE(hash);
    syncbox::BlobRecord blob;
    blob.content_hash = hash.value().to_hex();
    blob.size = static_cast<std::int64_t>(content.size());
    blob.chunk_size = static_cast<std::int64_t>(content.empty() ? 1 : content.size());
    blob.chunk_count = 1;
    blob.created_at = syncbox::now_millis();
    EXPECT_TRUE(store_->upsert_uploading_blob(blob));
    EXPECT_TRUE(store_->mark_blob_complete(blob.content_hash, syncbox::now_millis()));
    return hash.value();
  }

  syncbox::server::CommitRequest create_request(const std::string& path, const std::string& content,
                                                std::int64_t client_seq = 0) {
    syncbox::server::CommitRequest request;
    request.space_id = space_id_;
    request.device_id = "device-1";
    request.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
    request.kind = syncbox::OpKind::Create;
    request.base_revision = syncbox::Revision::none();
    request.path = path;
    request.content_hash = make_blob(content);
    request.size = static_cast<std::int64_t>(content.size());
    request.mtime_ms = 1;
    request.client_seq = syncbox::Seq{client_seq};
    request.request_hash = syncbox::generate_uuid_v7();
    return request;
  }

  [[nodiscard]] std::int64_t count(const std::string& table) {
    return syncbox::test::scalar_int(store_->db(),
                                     "SELECT COUNT(*) FROM " + table +
                                         " WHERE space_id='" + space_id_ + "';");
  }

  syncbox::test::TempDirectory directory_;
  std::optional<syncbox::ServerStore> store_;
  std::unique_ptr<syncbox::server::OpService> ops_;
  std::string space_id_;
};

TEST_F(OpServiceTest, ReplayDoesNotCreateAdditionalVersions) {
  auto request = create_request("a.txt", "one");
  auto first = ops_->commit(request);
  ASSERT_TRUE(succeeded(first)) << std::get<CommitFailure>(first).message;

  for (int i = 0; i < 100; ++i) {
    auto replay = ops_->commit(request);
    ASSERT_TRUE(succeeded(replay)) << std::get<CommitFailure>(replay).message;
    EXPECT_TRUE(std::get<CommitSuccess>(replay).replayed);
  }

  EXPECT_EQ(count("versions"), 1);
  EXPECT_EQ(count("change_log"), 1);
  EXPECT_EQ(count("operations"), 1);
}

TEST_F(OpServiceTest, StaleBaseRevisionAllowsExactlyOneWinner) {
  auto create = create_request("a.txt", "one");
  auto created = ops_->commit(create);
  ASSERT_TRUE(succeeded(created));
  const std::string file_id =
      std::get<CommitSuccess>(created).response["file_id"].get<std::string>();

  int successes = 0;
  int conflicts = 0;
  for (int i = 0; i < 10; ++i) {
    syncbox::server::CommitRequest update;
    update.space_id = space_id_;
    update.device_id = "device-" + std::to_string(i + 2);
    update.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
    update.kind = syncbox::OpKind::Update;
    update.file_id = syncbox::FileId{file_id};
    update.base_revision = syncbox::Revision{1};
    update.path = "a.txt";
    update.content_hash = make_blob("content-" + std::to_string(i));
    update.size = static_cast<std::int64_t>(("content-" + std::to_string(i)).size());
    update.mtime_ms = 2;
    update.client_seq = syncbox::Seq{0};
    update.request_hash = syncbox::generate_uuid_v7();

    auto outcome = ops_->commit(update);
    if (succeeded(outcome)) {
      ++successes;
    } else if (failure_code(outcome) == syncbox::ErrorCode::RevisionConflict) {
      ++conflicts;
    }
  }

  EXPECT_EQ(successes, 1);
  EXPECT_EQ(conflicts, 9);
  EXPECT_EQ(count("versions"), 2);

  auto entry = store_->find_entry(space_id_, syncbox::FileId{file_id});
  ASSERT_TRUE(entry);
  ASSERT_TRUE(entry.value().has_value());
  EXPECT_EQ(entry.value()->revision.value, 2);
}

TEST_F(OpServiceTest, OldClientCannotResurrectDeletedPath) {
  auto create = create_request("gone.txt", "one");
  auto created = ops_->commit(create);
  ASSERT_TRUE(succeeded(created));
  const std::string file_id =
      std::get<CommitSuccess>(created).response["file_id"].get<std::string>();

  syncbox::server::CommitRequest remove;
  remove.space_id = space_id_;
  remove.device_id = "device-1";
  remove.op_id = syncbox::OpId{syncbox::generate_uuid_v7()};
  remove.kind = syncbox::OpKind::Delete;
  remove.file_id = syncbox::FileId{file_id};
  remove.base_revision = syncbox::Revision{1};
  remove.path = "gone.txt";
  remove.client_seq = syncbox::Seq{0};
  remove.request_hash = syncbox::generate_uuid_v7();

  auto deleted = ops_->commit(remove);
  ASSERT_TRUE(succeeded(deleted));
  const auto delete_seq = std::get<CommitSuccess>(deleted).response["seq"].get<std::int64_t>();

  auto stale = create_request("gone.txt", "two", 0);
  auto blocked = ops_->commit(stale);
  ASSERT_FALSE(succeeded(blocked));
  EXPECT_EQ(failure_code(blocked), syncbox::ErrorCode::ResurrectBlocked);

  auto aware = create_request("gone.txt", "three", delete_seq);
  EXPECT_TRUE(succeeded(ops_->commit(aware)));
}

TEST_F(OpServiceTest, CaseFoldedPathsCollide) {
  auto first = create_request("Report.TXT", "one");
  ASSERT_TRUE(succeeded(ops_->commit(first)));

  auto second = create_request("report.txt", "two");
  auto outcome = ops_->commit(second);
  ASSERT_FALSE(succeeded(outcome));
  EXPECT_EQ(failure_code(outcome), syncbox::ErrorCode::PathCaseCollision);
}
