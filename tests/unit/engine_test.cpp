#include <gtest/gtest.h>

#include <syncbox/client/daemon.hpp>
#include <syncbox/client/engine.hpp>
#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_hub.hpp>
#include <syncbox/server/api.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "test_support.hpp"

namespace {

namespace http = boost::beast::http;

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::optional<std::filesystem::path> find_conflict_copy(
    const std::filesystem::path& root, const std::string& stem) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().u8string();
    if (name.find(stem + " (conflict ") == 0) {
      return entry.path();
    }
  }
  return std::nullopt;
}

}  // namespace

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto store = syncbox::ServerStore::open(directory_.file("server.db").string());
    ASSERT_TRUE(store);
    ASSERT_TRUE(store.value().migrate());
    store_ = std::move(store.value());

    space_id_ = syncbox::generate_uuid_v7();
    ASSERT_TRUE(store_->insert_space({space_id_, "Test", syncbox::now_millis(), "active"}));

    blobs_ = std::make_unique<syncbox::server::BlobStore>(*store_, directory_.file("blobs"));
    ASSERT_TRUE(blobs_->prepare());
    auth_ = std::make_unique<syncbox::server::AuthService>(*store_);
    ops_ = std::make_unique<syncbox::server::OpService>(*store_);
    api_ = std::make_unique<syncbox::server::Api>(*store_, *auth_, *ops_, *blobs_, hub_);
    server_ = std::make_unique<syncbox::net::HttpServer>("127.0.0.1", 0, *api_, 2);
    ASSERT_TRUE(server_->start());
    base_url_ = "http://127.0.0.1:" + std::to_string(server_->port());
  }

  void TearDown() override {
    server_.reset();
    api_.reset();
    ops_.reset();
    auth_.reset();
    blobs_.reset();
    store_.reset();
  }

  [[nodiscard]] std::string enroll(const std::string& name) {
    auto token = syncbox::generate_token();
    EXPECT_TRUE(token);
    auto hash = syncbox::hash_token(token.value());
    EXPECT_TRUE(hash);
    syncbox::EnrollmentTokenRecord record;
    record.token_hash.assign(hash.value().bytes.begin(), hash.value().bytes.end());
    record.space_id = space_id_;
    record.created_by = "test";
    record.expires_at = syncbox::now_millis() + 3600 * 1000;
    EXPECT_TRUE(store_->insert_enrollment_token(record));

    syncbox::net::HttpClient client(base_url_);
    nlohmann::json body{{"enrollment_token", token.value()},
                        {"device_name", name},
                        {"platform", "linux"},
                        {"app_version", "0.1.0"}};
    auto response = client.request(http::verb::post, "/v1/enroll", body.dump());
    EXPECT_TRUE(response);
    EXPECT_EQ(response.value().status, 201);
    return nlohmann::json::parse(response.value().body)["device_token"].get<std::string>();
  }

  struct ClientHandle {
    std::filesystem::path root;
    std::unique_ptr<syncbox::ClientStore> store;
    std::unique_ptr<syncbox::client::SyncEngine> engine;
  };

  [[nodiscard]] ClientHandle make_client(const std::string& name, const std::string& token,
                                         const std::string& server_url = {}) {
    ClientHandle handle;
    handle.root = directory_.path() / name;
    std::filesystem::create_directories(handle.root);

    auto store = syncbox::ClientStore::open(directory_.file(name + ".db").string());
    EXPECT_TRUE(store);
    EXPECT_TRUE(store.value().migrate());
    handle.store = std::make_unique<syncbox::ClientStore>(std::move(store.value()));

    syncbox::client::EngineConfig config;
    config.root = handle.root.string();
    config.server_url = server_url.empty() ? base_url_ : server_url;
    config.device_token = token;
    config.space_id = space_id_;
    handle.engine =
        std::make_unique<syncbox::client::SyncEngine>(config, *handle.store);
    return handle;
  }

  syncbox::test::TempDirectory directory_;
  std::optional<syncbox::ServerStore> store_;
  std::unique_ptr<syncbox::server::BlobStore> blobs_;
  std::unique_ptr<syncbox::server::AuthService> auth_;
  std::unique_ptr<syncbox::server::OpService> ops_;
  std::unique_ptr<syncbox::server::Api> api_;
  syncbox::net::WsHub hub_;
  std::unique_ptr<syncbox::net::HttpServer> server_;
  std::string base_url_;
  std::string space_id_;
};

TEST_F(EngineTest, PropagatesCreateUpdateAndDelete) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "hello.txt", "one");
  ASSERT_TRUE(alice.engine->run_once());
  EXPECT_FALSE(std::filesystem::exists(bob.root / "hello.txt"));

  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_TRUE(std::filesystem::exists(bob.root / "hello.txt"));
  EXPECT_EQ(read_file(bob.root / "hello.txt"), "one");

  write_file(alice.root / "hello.txt", "two-longer");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());
  EXPECT_EQ(read_file(bob.root / "hello.txt"), "two-longer");

  std::filesystem::rename(alice.root / "hello.txt", alice.root / "renamed.txt");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());
  EXPECT_FALSE(std::filesystem::exists(bob.root / "hello.txt"));
  EXPECT_EQ(read_file(bob.root / "renamed.txt"), "two-longer");

  std::filesystem::remove(alice.root / "renamed.txt");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());
  EXPECT_FALSE(std::filesystem::exists(bob.root / "hello.txt"));
}

TEST_F(EngineTest, OfflineQueueIsPersistedAndPushedLater) {
  auto alice = make_client("alice", enroll("alice"), "http://127.0.0.1:1");
  write_file(alice.root / "offline.txt", "queued");

  auto failed = alice.engine->run_once();
  EXPECT_FALSE(failed);

  auto pending = alice.store->list_pending_ops();
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending.value().size(), 1U);
  EXPECT_EQ(pending.value().front().state, "retry");

  syncbox::client::EngineConfig config;
  config.root = alice.root.string();
  config.server_url = base_url_;
  config.device_token = enroll("alice-online");
  config.space_id = space_id_;
  auto online = std::make_unique<syncbox::client::SyncEngine>(config, *alice.store);
  ASSERT_TRUE(online->run_once());

  auto after = alice.store->list_pending_ops();
  ASSERT_TRUE(after);
  EXPECT_TRUE(after.value().empty());
}

TEST_F(EngineTest, ConcurrentEditsProduceAConflictRecord) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "shared.txt", "base");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_EQ(read_file(bob.root / "shared.txt"), "base");

  write_file(alice.root / "shared.txt", "alice-edit");
  write_file(bob.root / "shared.txt", "bob-edit");

  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  auto conflicts = bob.store->list_unresolved_conflicts();
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts.value().size(), 1U);
  EXPECT_EQ(conflicts.value().front().resolution, "conflict_copy");
  EXPECT_EQ(read_file(bob.root / "shared.txt"), "alice-edit");

  auto copy = find_conflict_copy(bob.root, "shared");
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(read_file(*copy), "bob-edit");

  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_TRUE(alice.engine->run_once());
  auto alice_copy = find_conflict_copy(alice.root, "shared");
  ASSERT_TRUE(alice_copy.has_value());
  EXPECT_EQ(read_file(*alice_copy), "bob-edit");
}

TEST_F(EngineTest, DeleteLosesAgainstRemoteModification) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "doc.txt", "base");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  std::filesystem::remove(bob.root / "doc.txt");
  write_file(alice.root / "doc.txt", "alice-kept");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  ASSERT_TRUE(std::filesystem::exists(bob.root / "doc.txt"));
  EXPECT_EQ(read_file(bob.root / "doc.txt"), "alice-kept");
  auto conflicts = bob.store->list_unresolved_conflicts();
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts.value().size(), 1U);
  EXPECT_EQ(conflicts.value().front().resolution, "remote_wins");
}

TEST_F(EngineTest, LocalModificationRestoresRemoteDeletion) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "keep.txt", "base");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  std::filesystem::remove(alice.root / "keep.txt");
  write_file(bob.root / "keep.txt", "bob-kept");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  ASSERT_TRUE(std::filesystem::exists(bob.root / "keep.txt"));
  EXPECT_EQ(read_file(bob.root / "keep.txt"), "bob-kept");
  auto conflicts = bob.store->list_unresolved_conflicts();
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts.value().size(), 1U);
  EXPECT_EQ(conflicts.value().front().resolution, "restored");

  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(std::filesystem::exists(alice.root / "keep.txt"));
  EXPECT_EQ(read_file(alice.root / "keep.txt"), "bob-kept");
}

TEST_F(EngineTest, ConflictingCreatesBecomeConflictCopies) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "same.txt", "from-alice");
  write_file(bob.root / "same.txt", "from-bob");

  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  ASSERT_TRUE(std::filesystem::exists(bob.root / "same.txt"));
  EXPECT_EQ(read_file(bob.root / "same.txt"), "from-alice");
  auto copy = find_conflict_copy(bob.root, "same");
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(read_file(*copy), "from-bob");

  auto conflicts = bob.store->list_unresolved_conflicts();
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts.value().size(), 1U);
  EXPECT_EQ(conflicts.value().front().kind, "path_collision");

  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_TRUE(alice.engine->run_once());
  auto alice_copy = find_conflict_copy(alice.root, "same");
  ASSERT_TRUE(alice_copy.has_value());
  EXPECT_EQ(read_file(*alice_copy), "from-bob");
}


TEST_F(EngineTest, DaemonPollsRemoteChanges) {
  auto alice = make_client("alice", enroll("alice"));

  const std::string bob_token = enroll("bob");
  const auto bob_root = directory_.path() / "bob-daemon";
  std::filesystem::create_directories(bob_root);
  auto bob_store = syncbox::ClientStore::open(directory_.file("bob-daemon.db").string());
  ASSERT_TRUE(bob_store);
  ASSERT_TRUE(bob_store.value().migrate());

  syncbox::client::EngineConfig config;
  config.root = bob_root.string();
  config.server_url = base_url_;
  config.device_token = bob_token;
  config.space_id = space_id_;

  syncbox::client::SyncDaemon daemon(config, bob_store.value(),
                                     std::chrono::milliseconds(200));
  ASSERT_TRUE(daemon.start());

  write_file(alice.root / "pushed.txt", "from-alice");
  ASSERT_TRUE(alice.engine->run_once());

  bool arrived = false;
  for (int attempt = 0; attempt < 200 && !arrived; ++attempt) {
    if (std::filesystem::exists(bob_root / "pushed.txt") &&
        read_file(bob_root / "pushed.txt") == "from-alice") {
      arrived = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(arrived) << daemon.last_error();
  daemon.stop();
}


TEST_F(EngineTest, DaemonReceivesWebSocketHints) {
  auto alice = make_client("alice", enroll("alice"));

  const std::string bob_token = enroll("bob");
  const auto bob_root = directory_.path() / "bob-ws";
  std::filesystem::create_directories(bob_root);
  auto bob_store = syncbox::ClientStore::open(directory_.file("bob-ws.db").string());
  ASSERT_TRUE(bob_store);
  ASSERT_TRUE(bob_store.value().migrate());

  syncbox::client::EngineConfig config;
  config.root = bob_root.string();
  config.server_url = base_url_;
  config.device_token = bob_token;
  config.space_id = space_id_;

  // Deliberately long poll interval: only the WebSocket hint can trigger sync.
  syncbox::client::SyncDaemon daemon(config, bob_store.value(),
                                     std::chrono::seconds(30));
  ASSERT_TRUE(daemon.start());
  ASSERT_TRUE(daemon.wait_connected(std::chrono::seconds(3)));

  write_file(alice.root / "ws.txt", "hint");
  ASSERT_TRUE(alice.engine->run_once());

  bool arrived = false;
  for (int attempt = 0; attempt < 200 && !arrived; ++attempt) {
    if (std::filesystem::exists(bob_root / "ws.txt") &&
        read_file(bob_root / "ws.txt") == "hint") {
      arrived = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(arrived) << daemon.last_error();
  daemon.stop();
}


TEST_F(EngineTest, RecoverResetsInFlightOperation) {
  auto alice = make_client("alice", enroll("alice"), "http://127.0.0.1:1");
  write_file(alice.root / "recovered.txt", "recover-me");
  EXPECT_FALSE(alice.engine->run_once());

  auto pending = alice.store->list_pending_ops();
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending.value().size(), 1U);
  ASSERT_TRUE(alice.store->update_pending_state(pending.value().front().op_id, "committing",
                                                std::nullopt, std::nullopt));

  syncbox::client::EngineConfig config;
  config.root = alice.root.string();
  config.server_url = base_url_;
  config.device_token = enroll("alice-online");
  config.space_id = space_id_;
  auto online = std::make_unique<syncbox::client::SyncEngine>(config, *alice.store);
  ASSERT_TRUE(online->run_once());

  auto after = alice.store->list_pending_ops();
  ASSERT_TRUE(after);
  EXPECT_TRUE(after.value().empty());
}


TEST_F(EngineTest, ReconcileAppliesSnapshotWithoutUsingCursor) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  write_file(alice.root / "base.txt", "base");
  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());

  write_file(alice.root / "extra.txt", "extra");
  ASSERT_TRUE(alice.engine->run_once());

  ASSERT_FALSE(std::filesystem::exists(bob.root / "extra.txt"));
  ASSERT_TRUE(bob.engine->reconcile());
  ASSERT_TRUE(std::filesystem::exists(bob.root / "extra.txt"));
  EXPECT_EQ(read_file(bob.root / "extra.txt"), "extra");
}


TEST_F(EngineTest, PropagatesFileLargerThanOneChunk) {
  auto alice = make_client("alice", enroll("alice"));
  auto bob = make_client("bob", enroll("bob"));

  std::string content(5 * 1024 * 1024, '\0');
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>(i % 251);
  }
  write_file(alice.root / "large.bin", content);

  ASSERT_TRUE(alice.engine->run_once());
  ASSERT_TRUE(bob.engine->run_once());
  ASSERT_TRUE(std::filesystem::exists(bob.root / "large.bin"));
  EXPECT_EQ(std::filesystem::file_size(bob.root / "large.bin"), content.size());

  auto expected = syncbox::sha256(content);
  auto actual = syncbox::sha256_file(bob.root / "large.bin");
  ASSERT_TRUE(expected);
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual.value(), expected.value());
}
