// Manual benchmark (PF-01): single-file engine round trip between two clients
// through the in-process server. Measures write -> remote applied, including
// scan, blob upload, commit, change pull and apply.
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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

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

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 20;
  if (argc >= 2) {
    iterations = std::atoi(argv[1]);
    if (iterations <= 0) {
      std::cerr << "iterations must be positive\n";
      return 2;
    }
  }

  const auto unique = now_ms();
  const auto root = std::filesystem::temp_directory_path() /
                    ("syncbox-latency-bench-" + std::to_string(unique));
  std::filesystem::create_directories(root);

  auto store_result = syncbox::ServerStore::open((root / "server.db").string());
  if (!store_result || !store_result.value().migrate()) {
    std::cerr << "server store init failed\n";
    return 1;
  }
  auto& store = store_result.value();
  const std::string space_id = syncbox::generate_uuid_v7();
  if (!store.insert_space({space_id, "bench", syncbox::now_millis(), "active"})) {
    return 1;
  }

  syncbox::server::BlobStore blobs(store, root / "blobs");
  if (!blobs.prepare()) {
    return 1;
  }
  syncbox::server::AuthService auth(store);
  syncbox::server::OpService ops(store);
  syncbox::net::WsHub hub;
  syncbox::server::Api api(store, auth, ops, blobs, hub);
  syncbox::net::HttpServer server("127.0.0.1", 0, api, 2);
  if (!server.start()) {
    return 1;
  }
  const std::string base_url = "http://127.0.0.1:" + std::to_string(server.port());

  const std::string alice_token = enroll(store, space_id, base_url, "alice");
  const std::string bob_token = enroll(store, space_id, base_url, "bob");
  if (alice_token.empty() || bob_token.empty()) {
    return 1;
  }

  struct Client {
    std::unique_ptr<syncbox::ClientStore> store;
    std::unique_ptr<syncbox::client::SyncEngine> engine;
    std::filesystem::path root;
  };

  auto make_client = [&](const std::string& name, const std::string& token) {
    Client client;
    client.root = root / name;
    std::filesystem::create_directories(client.root);
    auto database = syncbox::ClientStore::open((root / (name + ".db")).string());
    if (!database || !database.value().migrate()) {
      return client;
    }
    client.store = std::make_unique<syncbox::ClientStore>(std::move(database.value()));
    syncbox::client::EngineConfig config;
    config.root = client.root.string();
    config.server_url = base_url;
    config.device_token = token;
    config.space_id = space_id;
    client.engine =
        std::make_unique<syncbox::client::SyncEngine>(config, *client.store);
    return client;
  };

  Client alice = make_client("alice", alice_token);
  Client bob = make_client("bob", bob_token);
  if (!alice.engine || !bob.engine) {
    std::cerr << "client init failed\n";
    return 1;
  }

  const auto path = alice.root / "latency.txt";
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(iterations));

  for (int i = 0; i < iterations; ++i) {
    const std::string content = "iteration-" + std::to_string(i) + "\n";
    {
      std::ofstream stream(path, std::ios::binary | std::ios::trunc);
      stream << content;
    }

    const auto start = now_ms();
    if (!alice.engine->run_once() || !bob.engine->run_once() ||
        read_file(bob.root / "latency.txt") != content) {
      std::cerr << "sync failed at iteration " << i << '\n';
      server.stop();
      std::filesystem::remove_all(root);
      return 1;
    }
    samples.push_back(now_ms() - start);
  }

  std::sort(samples.begin(), samples.end());
  const auto percentile = [&](double p) {
    const auto index = static_cast<std::size_t>(p * (samples.size() - 1));
    return samples[index];
  };

  std::cout << "iterations=" << iterations << " p50_ms=" << percentile(0.50)
            << " p95_ms=" << percentile(0.95) << " max_ms=" << samples.back() << '\n';

  server.stop();
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return 0;
}
