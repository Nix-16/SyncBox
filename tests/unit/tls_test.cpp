#include <gtest/gtest.h>

#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>

#include <boost/beast/http/verb.hpp>

#include <string>

namespace {

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
