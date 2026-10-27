#include <gtest/gtest.h>

#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/net/http_server.hpp>
#include <syncbox/server/api.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "test_support.hpp"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

struct ClientResponse {
  unsigned status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

[[nodiscard]] ClientResponse http_call(std::uint16_t port, http::verb method,
                                       const std::string& target, const std::string& body = {},
                                       const std::string& token = {}) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  beast::tcp_stream stream(io_context);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));

  http::request<http::string_body> request{method, target, 11};
  request.set(http::field::host, "127.0.0.1");
  request.set(http::field::content_type, "application/json");
  if (!token.empty()) {
    request.set(http::field::authorization, "Bearer " + token);
  }
  request.body() = body;
  request.prepare_payload();
  http::write(stream, request);

  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);

  ClientResponse result;
  result.status = response.result_int();
  result.body = response.body();
  for (const auto& field : response) {
    std::string name(field.name_string());
    for (char& c : name) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    result.headers[name] = std::string(field.value());
  }
  return result;
}

}  // namespace

class ServerApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto store = syncbox::ServerStore::open(directory_.file("server.db").string());
    ASSERT_TRUE(store);
    ASSERT_TRUE(store.value().migrate());
    store_ = std::move(store.value());

    space_id_ = syncbox::generate_uuid_v7();
    syncbox::SpaceRecord space{space_id_, "Test", syncbox::now_millis(), "active"};
    ASSERT_TRUE(store_->insert_space(space));

    auto enrollment = syncbox::generate_token();
    ASSERT_TRUE(enrollment);
    enrollment_token_ = enrollment.value();
    auto enrollment_hash = syncbox::hash_token(enrollment_token_);
    ASSERT_TRUE(enrollment_hash);

    syncbox::EnrollmentTokenRecord record;
    record.token_hash.assign(enrollment_hash.value().bytes.begin(),
                             enrollment_hash.value().bytes.end());
    record.space_id = space_id_;
    record.created_by = "test";
    record.expires_at = syncbox::now_millis() + 3600 * 1000;
    ASSERT_TRUE(store_->insert_enrollment_token(record));

    auth_ = std::make_unique<syncbox::server::AuthService>(*store_);
    ops_ = std::make_unique<syncbox::server::OpService>(*store_);
    api_ = std::make_unique<syncbox::server::Api>(*store_, *auth_, *ops_);
    server_ = std::make_unique<syncbox::net::HttpServer>("127.0.0.1", 0, *api_, 2);
    ASSERT_TRUE(server_->start());
  }

  void TearDown() override {
    server_.reset();
    api_.reset();
    ops_.reset();
    auth_.reset();
    store_.reset();
  }

  [[nodiscard]] std::string enroll_device() {
    nlohmann::json body{{"enrollment_token", enrollment_token_},
                        {"device_name", "test-device"},
                        {"platform", "linux"},
                        {"app_version", "0.1.0"}};
    auto response = http_call(server_->port(), http::verb::post, "/v1/enroll", body.dump());
    EXPECT_EQ(response.status, 201);
    auto parsed = nlohmann::json::parse(response.body);
    device_id_ = parsed["device_id"].get<std::string>();
    return parsed["device_token"].get<std::string>();
  }

  [[nodiscard]] syncbox::ContentHash make_blob(const std::string& content) {
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

  syncbox::test::TempDirectory directory_;
  std::optional<syncbox::ServerStore> store_;
  std::unique_ptr<syncbox::server::AuthService> auth_;
  std::unique_ptr<syncbox::server::OpService> ops_;
  std::unique_ptr<syncbox::server::Api> api_;
  std::unique_ptr<syncbox::net::HttpServer> server_;
  std::string space_id_;
  std::string enrollment_token_;
  std::string device_id_;
};

TEST_F(ServerApiTest, EnrollAndMe) {
  const std::string token = enroll_device();
  ASSERT_FALSE(token.empty());

  auto me = http_call(server_->port(), http::verb::get, "/v1/me", {}, token);
  ASSERT_EQ(me.status, 200);
  auto body = nlohmann::json::parse(me.body);
  EXPECT_EQ(body["device_id"].get<std::string>(), device_id_);
  EXPECT_EQ(body["space_id"].get<std::string>(), space_id_);

  auto invalid = http_call(server_->port(), http::verb::get, "/v1/me", {}, "invalid-token");
  EXPECT_EQ(invalid.status, 401);

  auto reused = http_call(server_->port(), http::verb::post, "/v1/enroll",
                          nlohmann::json{{"enrollment_token", enrollment_token_},
                                         {"device_name", "again"},
                                         {"platform", "linux"}}
                              .dump());
  EXPECT_EQ(reused.status, 401);
}

TEST_F(ServerApiTest, CommitChangesSnapshotAndAck) {
  const std::string token = enroll_device();
  const auto hash = make_blob("hello");
  const std::string op_id = syncbox::generate_uuid_v7();

  nlohmann::json create{{"op_id", op_id},
                        {"kind", "create"},
                        {"file_id", nullptr},
                        {"base_revision", 0},
                        {"path", "docs/a.txt"},
                        {"content_hash", hash.to_hex()},
                        {"size", 5},
                        {"mtime_ms", 1},
                        {"client_seq", 0},
                        {"conflict", false}};
  const std::string create_body = create.dump();

  auto created = http_call(server_->port(), http::verb::post,
                           "/v1/spaces/" + space_id_ + "/ops", create_body, token);
  ASSERT_EQ(created.status, 200) << created.body;
  auto created_json = nlohmann::json::parse(created.body);
  const std::string file_id = created_json["file_id"].get<std::string>();
  EXPECT_EQ(created_json["revision"].get<std::int64_t>(), 1);
  EXPECT_EQ(created_json["path"].get<std::string>(), "docs/a.txt");

  auto replay = http_call(server_->port(), http::verb::post,
                          "/v1/spaces/" + space_id_ + "/ops", create_body, token);
  ASSERT_EQ(replay.status, 200) << replay.body;
  EXPECT_EQ(replay.headers["x-idempotent-replay"], "true");
  EXPECT_EQ(nlohmann::json::parse(replay.body)["revision"].get<std::int64_t>(), 1);

  const auto hash2 = make_blob("hello world");
  nlohmann::json update{{"op_id", syncbox::generate_uuid_v7()},
                        {"kind", "update"},
                        {"file_id", file_id},
                        {"base_revision", 1},
                        {"path", "docs/a.txt"},
                        {"content_hash", hash2.to_hex()},
                        {"size", 11},
                        {"mtime_ms", 2},
                        {"client_seq", 1},
                        {"conflict", false}};
  auto updated = http_call(server_->port(), http::verb::post,
                           "/v1/spaces/" + space_id_ + "/ops", update.dump(), token);
  ASSERT_EQ(updated.status, 200) << updated.body;
  EXPECT_EQ(nlohmann::json::parse(updated.body)["revision"].get<std::int64_t>(), 2);

  auto changes =
      http_call(server_->port(), http::verb::get,
                "/v1/spaces/" + space_id_ + "/changes?since=0", {}, token);
  ASSERT_EQ(changes.status, 200) << changes.body;
  auto changes_json = nlohmann::json::parse(changes.body);
  ASSERT_EQ(changes_json["changes"].size(), 2U);
  EXPECT_EQ(changes_json["changes"][0]["op"].get<std::string>(), "create");
  EXPECT_EQ(changes_json["changes"][1]["op"].get<std::string>(), "update");

  auto snapshot = http_call(server_->port(), http::verb::get,
                            "/v1/spaces/" + space_id_ + "/snapshot", {}, token);
  ASSERT_EQ(snapshot.status, 200) << snapshot.body;
  auto snapshot_json = nlohmann::json::parse(snapshot.body);
  ASSERT_EQ(snapshot_json["entries"].size(), 1U);
  EXPECT_EQ(snapshot_json["entries"][0]["revision"].get<std::int64_t>(), 2);

  auto entry = http_call(server_->port(), http::verb::get,
                         "/v1/spaces/" + space_id_ + "/entries/" + file_id, {}, token);
  ASSERT_EQ(entry.status, 200) << entry.body;
  EXPECT_EQ(nlohmann::json::parse(entry.body)["path"].get<std::string>(), "docs/a.txt");

  const auto last_seq = changes_json["changes"][1]["seq"].get<std::int64_t>();
  auto ack = http_call(server_->port(), http::verb::post,
                       "/v1/devices/" + device_id_ + "/ack",
                       nlohmann::json{{"last_seq", last_seq}}.dump(), token);
  EXPECT_EQ(ack.status, 204);
}

TEST_F(ServerApiTest, StaleRevisionReturnsConflict) {
  const std::string token = enroll_device();
  const auto hash = make_blob("one");

  nlohmann::json create{{"op_id", syncbox::generate_uuid_v7()},
                        {"kind", "create"},
                        {"file_id", nullptr},
                        {"base_revision", 0},
                        {"path", "a.txt"},
                        {"content_hash", hash.to_hex()},
                        {"size", 3},
                        {"mtime_ms", 1},
                        {"client_seq", 0},
                        {"conflict", false}};
  auto created = http_call(server_->port(), http::verb::post,
                           "/v1/spaces/" + space_id_ + "/ops", create.dump(), token);
  ASSERT_EQ(created.status, 200) << created.body;
  const std::string file_id = nlohmann::json::parse(created.body)["file_id"].get<std::string>();

  const auto hash2 = make_blob("two");
  nlohmann::json first_update{{"op_id", syncbox::generate_uuid_v7()},
                              {"kind", "update"},
                              {"file_id", file_id},
                              {"base_revision", 1},
                              {"path", "a.txt"},
                              {"content_hash", hash2.to_hex()},
                              {"size", 3},
                              {"mtime_ms", 2},
                              {"client_seq", 0},
                              {"conflict", false}};
  auto ok = http_call(server_->port(), http::verb::post,
                      "/v1/spaces/" + space_id_ + "/ops", first_update.dump(), token);
  ASSERT_EQ(ok.status, 200) << ok.body;

  nlohmann::json stale = first_update;
  stale["op_id"] = syncbox::generate_uuid_v7();
  stale["base_revision"] = 1;  // now stale: current revision is 2
  auto conflict = http_call(server_->port(), http::verb::post,
                            "/v1/spaces/" + space_id_ + "/ops", stale.dump(), token);
  ASSERT_EQ(conflict.status, 409) << conflict.body;
  auto body = nlohmann::json::parse(conflict.body);
  EXPECT_EQ(body["error"]["code"].get<std::string>(), "REVISION_CONFLICT");
  EXPECT_EQ(body["error"]["details"]["current"]["revision"].get<std::int64_t>(), 2);
}
