#pragma once

#include <syncbox/core/result.hpp>

#include <boost/beast/http/verb.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syncbox::net {

struct ClientResponse {
  unsigned status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

struct TlsOptions {
  bool verify = true;
  std::string ca_file;
  std::string server_name;
};

// Minimal synchronous HTTP/1.1 client used by the sync engine. Supports
// http:// and https:// (TLS with optional CA file and hostname verification).
class HttpClient {
 public:
  explicit HttpClient(std::string base_url, TlsOptions tls = {});

  [[nodiscard]] Result<ClientResponse> request(
      boost::beast::http::verb method, std::string_view target,
      std::string_view body = {}, std::string_view token = {},
      std::vector<std::pair<std::string, std::string>> headers = {});

  // Streams the response body to `destination`; returns the HTTP status.
  // With offset > 0 it sends "Range: bytes=<offset>-" and appends to the file,
  // so a partial download can be resumed (expects 206; 200 means the server
  // ignored the range and the caller must restart).
  [[nodiscard]] Result<unsigned> download_to_file(
      std::string_view target, std::string_view token,
      const std::filesystem::path& destination, std::int64_t offset = 0);

  [[nodiscard]] const std::string& base_url() const noexcept { return base_url_; }

 private:
  [[nodiscard]] std::string server_name() const;

  std::string base_url_;
  std::string host_;
  std::string port_;
  std::string prefix_;
  bool secure_ = false;
  TlsOptions tls_;
};

}  // namespace syncbox::net
