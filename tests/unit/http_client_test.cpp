#include <gtest/gtest.h>

#include <syncbox/net/http_client.hpp>
#include <syncbox/net/http_server.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "test_support.hpp"

namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Serves a fixed 10-byte body and honours a single "bytes=N-" range.
class RangeHandler final : public syncbox::net::RequestHandler {
 public:
  syncbox::net::HttpResponse handle(const syncbox::net::HttpRequest& request) override {
    syncbox::net::HttpResponse response;
    response.content_type = "application/octet-stream";
    if (request.target != "/data") {
      response.status = 404;
      return response;
    }

    const std::string body = "0123456789";
    std::int64_t offset = 0;
    for (const auto& [name, value] : request.headers) {
      if (lower(name) != "range") {
        continue;
      }
      constexpr std::string_view prefix = "bytes=";
      if (value.rfind(prefix, 0) == 0) {
        const auto dash = value.find('-', prefix.size());
        if (dash != std::string::npos) {
          offset = std::stoll(value.substr(prefix.size(), dash - prefix.size()));
        }
      }
    }
    if (offset < 0 || offset > static_cast<std::int64_t>(body.size())) {
      response.status = 416;
      return response;
    }

    response.status = offset > 0 ? 206 : 200;
    response.body = body.substr(static_cast<std::size_t>(offset));
    if (offset > 0) {
      response.headers.emplace_back(
          "Content-Range", "bytes " + std::to_string(offset) + "-9/10");
    }
    return response;
  }
};

}  // namespace

TEST(HttpClientTest, DownloadsFullBodyWithoutRange) {
  RangeHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler, 2);
  ASSERT_TRUE(server.start());

  syncbox::test::TempDirectory directory;
  const auto destination = directory.file("full.bin");

  syncbox::net::HttpClient client("http://127.0.0.1:" + std::to_string(server.port()));
  auto status = client.download_to_file("/data", "", destination);
  ASSERT_TRUE(status) << status.error().message;
  EXPECT_EQ(status.value(), 200U);
  EXPECT_EQ(read_file(destination), "0123456789");
  server.stop();
}

TEST(HttpClientTest, ResumesWithRangeAndAppends) {
  RangeHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler, 2);
  ASSERT_TRUE(server.start());

  syncbox::test::TempDirectory directory;
  const auto destination = directory.file("partial.bin");
  {
    std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
    stream << "0123";
  }

  syncbox::net::HttpClient client("http://127.0.0.1:" + std::to_string(server.port()));
  auto status = client.download_to_file("/data", "", destination, 4);
  ASSERT_TRUE(status) << status.error().message;
  EXPECT_EQ(status.value(), 206U);
  EXPECT_EQ(read_file(destination), "0123456789");
  server.stop();
}
