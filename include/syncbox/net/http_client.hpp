#pragma once

#include <syncbox/core/result.hpp>

#include <boost/beast/http/verb.hpp>

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

// Minimal synchronous HTTP/1.1 client used by the sync engine. TLS is
// scheduled for M6; only http:// URLs are accepted here.
class HttpClient {
 public:
  explicit HttpClient(std::string base_url, TlsOptions tls = {});

  [[nodiscard]] Result<ClientResponse> request(
      boost::beast::http::verb method, std::string_view target,
      std::string_view body = {}, std::string_view token = {},
      std::vector<std::pair<std::string, std::string>> headers = {});

  // Streams the response body to `destination`; returns the HTTP status.
  [[nodiscard]] Result<unsigned> download_to_file(
      std::string_view target, std::string_view token,
      const std::filesystem::path& destination);

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
