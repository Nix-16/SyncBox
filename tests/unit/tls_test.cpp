#include <gtest/gtest.h>

#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_client.hpp>
#include <syncbox/net/ws_hub.hpp>

#include <boost/beast/http/verb.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

using syncbox::Seq;

namespace http = boost::beast::http;

class HealthHandler final : public syncbox::net::RequestHandler {
 public:
  syncbox::net::HttpResponse handle(const syncbox::net::HttpRequest& request) override {
    syncbox::net::HttpResponse response;
    if (request.target == "/v1/health") {
      response.status = 200;
      response.body = R"({"status":"ok"})";
    } else {
      response.status = 404;
      response.body = R"({"error":{"code":"NOT_FOUND"}})";
    }
    return response;
  }
};

// Accepts WebSocket upgrades over both plaintext and TLS, without auth, so
// the transport itself can be tested in isolation.
class WsTestHandler final : public syncbox::net::RequestHandler {
 public:
  syncbox::net::HttpResponse handle(const syncbox::net::HttpRequest&) override {
    syncbox::net::HttpResponse response;
    response.status = 404;
    return response;
  }

  syncbox::net::UpgradeResult handle_upgrade(
      boost::beast::tcp_stream&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const syncbox::net::HttpRequest&) override {
    auto session = std::make_shared<
        syncbox::net::WsSession<boost::beast::tcp_stream>>(std::move(stream), hub_, "space",
                                                           Seq{0});
    hub_.add(session);
    session->run(std::move(request));
    return syncbox::net::UpgradeResult{true, {}};
  }

  syncbox::net::UpgradeResult handle_upgrade(
      boost::beast::ssl_stream<boost::beast::tcp_stream>&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const syncbox::net::HttpRequest&) override {
    using Stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;
    auto session =
        std::make_shared<syncbox::net::WsSession<Stream>>(std::move(stream), hub_, "space",
                                                          Seq{0});
    hub_.add(session);
    session->run(std::move(request));
    return syncbox::net::UpgradeResult{true, {}};
  }

  syncbox::net::WsHub hub_;
};

[[nodiscard]] std::string certificate_path() {
  return std::string(SYNCBOX_SOURCE_DIR) + "/tests/data/test-server.crt";
}

[[nodiscard]] std::string key_path() {
  return std::string(SYNCBOX_SOURCE_DIR) + "/tests/data/test-server.key";
}

}  // namespace

TEST(TlsTest, ServesHealthOverTlsWithCaVerification) {
  HealthHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler,
                                  syncbox::net::TlsConfig{certificate_path(), key_path()}, 1);
  ASSERT_TRUE(server.start());

  syncbox::net::HttpClient client(
      "https://127.0.0.1:" + std::to_string(server.port()),
      syncbox::net::TlsOptions{true, certificate_path(), "localhost"});
  auto response = client.request(http::verb::get, "/v1/health");
  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response.value().status, 200);
  EXPECT_EQ(response.value().body, R"({"status":"ok"})");

  server.stop();
}

TEST(TlsTest, WebSocketOverTlsDeliversChanges) {
  WsTestHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler,
                                  syncbox::net::TlsConfig{certificate_path(), key_path()}, 2);
  ASSERT_TRUE(server.start());

  std::mutex mutex;
  std::condition_variable condition;
  std::optional<Seq> received;

  syncbox::net::WsClient client(
      "wss://127.0.0.1:" + std::to_string(server.port()), "space", "token",
      [&](Seq seq) {
        std::lock_guard<std::mutex> lock(mutex);
        received = seq;
        condition.notify_all();
      },
      syncbox::net::TlsOptions{true, certificate_path(), "localhost"});

  std::thread thread([&] { client.run(); });
  ASSERT_TRUE(client.wait_connected(std::chrono::seconds(5)));

  handler.hub_.notify_change("space", Seq{42});

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return received.has_value(); }));
    EXPECT_EQ(received->value, 42);
  }

  client.stop();
  thread.join();
  server.stop();
}

TEST(TlsTest, RejectsUntrustedCertificate) {
  HealthHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler,
                                  syncbox::net::TlsConfig{certificate_path(), key_path()}, 1);
  ASSERT_TRUE(server.start());

  syncbox::net::HttpClient client(
      "https://127.0.0.1:" + std::to_string(server.port()),
      syncbox::net::TlsOptions{true, "", "localhost"});
  auto response = client.request(http::verb::get, "/v1/health");
  EXPECT_FALSE(response);

  server.stop();
}
