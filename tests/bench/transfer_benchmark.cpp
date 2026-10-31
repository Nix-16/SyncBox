// Manual benchmark (PF-02): uploads and downloads one 100 MiB file through the
// in-process server and the real sync engine. Not registered with CTest.
#include <syncbox/client/engine.hpp>
#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_hub.hpp>
#include <syncbox/server/api.hpp>
#include <syncbox/server/auth_service.hpp>
#include <syncbox/server/blob_store.hpp>
#include <syncbox/server/op_service.hpp>
#include <syncbox/store/client_store.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

constexpr std::int64_t kMiB = 1024 * 1024;

[[nodiscard]] std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string enroll(syncbox::ServerStore& store, const std::string& space_id,
                                 const std::string& base_url, const std::string& name) {
  auto token = syncbox::generate_token();
  auto hash = syncbox::hash_token(token.value());
  syncbox::EnrollmentTokenRecord record;
  record.token_hash.assign(hash.value().bytes.begin(), hash.value().bytes.end());
  record.space_id = space_id;
  record.created_by = "benchmark";
  record.expires_at = syncbox::now_millis() + 3600 * 1000;
  if (!store.insert_enrollment_token(record)) {
    return {};
  }

  syncbox::net::HttpClient client(base_url);
  nlohmann::json body{{"enrollment_token", token.value()},
                      {"device_name", name},
                      {"platform", "linux"},
                      {"app_version", "benchmark"}};
  auto response = client.request(boost::beast::http::verb::post, "/v1/enroll", body.dump());
  if (!response || response.value().status != 201) {
    return {};
  }
  return nlohmann::json::parse(response.value().body)["device_token"].get<std::string>();
}

}  // namespace

int main(int argc, char** argv) {
  std::int64_t megabytes = 100;
  if (argc >= 2) {
    megabytes = std::atoll(argv[1]);
    if (megabytes <= 0) {
      std::cerr << "size must be positive\n";
      return 2;
    }
  }
  const std::int64_t size = megabytes * kMiB;

  const auto unique = now_ms();
  const auto root = std::filesystem::temp_directory_path() /
                    ("syncbox-transfer-bench-" + std::to_string(unique));
  std::filesystem::create_directories(root);

  auto store_result = syncbox::ServerStore::open((root / "server.db").string());
  if (!store_result || !store_result.value().migrate()) {
    std::cerr << "server store init failed\n";
    return 1;
  }
  auto& store = store_result.value();

  const std::string space_id = syncbox::generate_uuid_v7();
  if (!store.insert_space({space_id, "bench", syncbox::now_millis(), "active"})) {
    std::cerr << "space insert failed\n";
    return 1;
  }

  syncbox::server::BlobStore blobs(store, root / "blobs");
  if (!blobs.prepare()) {
    std::cerr << "blob store init failed\n";
    return 1;
  }
  syncbox::server::AuthService auth(store);
  syncbox::server::OpService ops(store);
  syncbox::net::WsHub hub;
  syncbox::server::Api api(store, auth, ops, blobs, hub);
  syncbox::net::HttpServer server("127.0.0.1", 0, api, 2);
  if (!server.start()) {
    std::cerr << "server start failed\n";
    return 1;
  }
  const std::string base_url = "http://127.0.0.1:" + std::to_string(server.port());

  const std::string alice_token = enroll(store, space_id, base_url, "alice");
  const std::string bob_token = enroll(store, space_id, base_url, "bob");
  if (alice_token.empty() || bob_token.empty()) {
    std::cerr << "enrollment failed\n";
    return 1;
  }

  auto make_client = [&](const std::string& name, const std::string& token)
      -> std::pair<std::unique_ptr<syncbox::ClientStore>,
                   std::unique_ptr<syncbox::client::SyncEngine>> {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    auto client_store = syncbox::ClientStore::open((root / (name + ".db")).string());
    if (!client_store || !client_store.value().migrate()) {
      return {nullptr, nullptr};
    }
    auto store_ptr =
        std::make_unique<syncbox::ClientStore>(std::move(client_store.value()));
    syncbox::client::EngineConfig config;
    config.root = directory.string();
    config.server_url = base_url;
    config.device_token = token;
    config.space_id = space_id;
    auto engine =
        std::make_unique<syncbox::client::SyncEngine>(config, *store_ptr);
    return {std::move(store_ptr), std::move(engine)};
  };

  auto alice = make_client("alice", alice_token);
  auto bob = make_client("bob", bob_token);
  if (!alice.first || !alice.second || !bob.first || !bob.second) {
    std::cerr << "client init failed\n";
    return 1;
  }

  const auto source = root / "alice" / "payload.bin";
  {
    std::ofstream stream(source, std::ios::binary | std::ios::trunc);
    std::string block(static_cast<std::size_t>(kMiB), '\0');
    for (std::size_t i = 0; i < block.size(); ++i) {
      block[i] = static_cast<char>(i % 251);
    }
    for (std::int64_t i = 0; i < megabytes; ++i) {
      stream.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
  }

  const auto upload_start = now_ms();
  const auto uploaded = alice.second->run_once();
  const auto upload_ms = now_ms() - upload_start;

  const auto download_start = now_ms();
  const auto downloaded = bob.second->run_once();
  const auto download_ms = now_ms() - download_start;

  bool hash_ok = false;
  if (uploaded && downloaded) {
    auto expected = syncbox::sha256_file(source);
    auto actual = syncbox::sha256_file(root / "bob" / "payload.bin");
    hash_ok = expected && actual && expected.value() == actual.value();
  }

  std::cout << "bytes=" << size << " upload_ms=" << upload_ms
            << " download_ms=" << download_ms << " hash_ok=" << (hash_ok ? 1 : 0)
            << '\n';

  server.stop();
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return (uploaded && downloaded && hash_ok) ? 0 : 1;
}
