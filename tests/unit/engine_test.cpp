#include <gtest/gtest.h>

#include <syncbox/client/engine.hpp>
#include <syncbox/common/time.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_hub.hpp>
#include <syncbox/server/api.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <boost/beast/http/verb.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

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
  EXPECT_EQ(conflicts.value().size(), 1U);
  EXPECT_EQ(read_file(bob.root / "shared.txt"), "bob-edit");
}
