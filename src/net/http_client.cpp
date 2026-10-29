#include <syncbox/net/http_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/optional.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace syncbox::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using asio::ip::tcp;

void parse_url(const std::string& url, std::string& host, std::string& port,
               std::string& prefix, bool& secure) {
  std::string_view rest = url;
  constexpr std::string_view kHttp = "http://";
  constexpr std::string_view kHttps = "https://";
  if (rest.rfind(kHttps, 0) == 0) {
    secure = true;
    rest.remove_prefix(kHttps.size());
  } else if (rest.rfind(kHttp, 0) == 0) {
    secure = false;
    rest.remove_prefix(kHttp.size());
  }

  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
  prefix = slash == std::string_view::npos ? std::string{} : std::string(rest.substr(slash));

  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    host = std::string(authority);
    port = secure ? "443" : "80";
  } else {
    host = std::string(authority.substr(0, colon));
    port = std::string(authority.substr(colon + 1));
  }
  if (host.empty()) {
    host = "127.0.0.1";
  }
}

template <typename Stream>
Result<ClientResponse> perform_request(Stream& stream,
                                       http::request<http::string_body>&& request) {
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.body_limit(64 * 1024 * 1024);
  http::read(stream, buffer, parser);
  auto response = parser.release();

  ClientResponse result;
  result.status = response.result_int();
  result.body = response.body();
  for (const auto& field : response) {
    std::string name(field.name_string());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    result.headers[name] = std::string(field.value());
  }
  return result;
}

template <typename Stream>
Result<unsigned> perform_download(Stream& stream, http::request<http::empty_body>&& request,
                                  const std::filesystem::path& destination) {
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response_parser<http::file_body> parser;
  parser.body_limit(boost::none);
  boost::system::error_code error;
  const std::string native_path = destination.u8string();
  parser.get().body().open(native_path.c_str(), beast::file_mode::write, error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot open download destination");
  }
  http::read(stream, buffer, parser);
  return parser.get().result_int();
}

}  // namespace

HttpClient::HttpClient(std::string base_url, TlsOptions tls)
    : base_url_(std::move(base_url)), tls_(std::move(tls)) {
  parse_url(base_url_, host_, port_, prefix_, secure_);
}

std::string HttpClient::server_name() const {
  return tls_.server_name.empty() ? host_ : tls_.server_name;
}

Result<unsigned> HttpClient::download_to_file(std::string_view target, std::string_view token,
                                              const std::filesystem::path& destination) {
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(host_, port_);

    http::request<http::empty_body> request{http::verb::get, std::string(target), 11};
    request.set(http::field::host, host_);
    if (!token.empty()) {
      request.set(http::field::authorization, "Bearer " + std::string(token));
    }
    request.prepare_payload();

    if (secure_) {
      ssl::context context(ssl::context::tls_client);
      boost::system::error_code error;
      if (!tls_.ca_file.empty()) {
        context.load_verify_file(tls_.ca_file, error);
      } else {
        context.set_default_verify_paths(error);
      }
      if (error) {
        return make_error(ErrorCode::IoError, "cannot load CA certificates");
      }

      beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
      stream.set_verify_mode(tls_.verify ? ssl::verify_peer : ssl::verify_none);
      if (tls_.verify) {
        stream.set_verify_callback(ssl::host_name_verification(server_name()));
      }
      const std::string name = server_name();
      SSL_ctrl(stream.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
               TLSEXT_NAMETYPE_host_name, const_cast<char*>(name.c_str()));
      beast::get_lowest_layer(stream).connect(endpoints, error);
      if (error) {
        return make_error(ErrorCode::Unavailable, "cannot connect: " + error.message());
      }
      stream.handshake(ssl::stream_base::client, error);
      if (error) {
        return make_error(ErrorCode::Unavailable, "TLS handshake failed: " + error.message());
      }

      auto result = perform_download(stream, std::move(request), destination);
      boost::system::error_code ignored;
      stream.shutdown(ignored);
      return result;
    }

    beast::tcp_stream stream(io_context);
    boost::system::error_code error;
    stream.connect(endpoints, error);
    if (error) {
      return make_error(ErrorCode::Unavailable, "cannot connect: " + error.message());
    }
    auto result = perform_download(stream, std::move(request), destination);
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return result;
  } catch (const std::exception& exception) {
    return make_error(ErrorCode::IoError,
                      std::string("HTTP download failed: ") + exception.what());
  }
}

Result<ClientResponse> HttpClient::request(
    http::verb method, std::string_view target, std::string_view body, std::string_view token,
    std::vector<std::pair<std::string, std::string>> headers) {
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(host_, port_);

    http::request<http::string_body> request{method, std::string(target), 11};
    request.set(http::field::host, host_);
    request.set(http::field::user_agent, "syncbox/0.1.0");
    if (!body.empty() || method == http::verb::post || method == http::verb::put) {
      request.set(http::field::content_type, "application/json");
      request.body() = std::string(body);
    }
    if (!token.empty()) {
      request.set(http::field::authorization, "Bearer " + std::string(token));
    }
    for (const auto& header : headers) {
      request.set(header.first, header.second);
    }
    request.prepare_payload();

    if (secure_) {
      ssl::context context(ssl::context::tls_client);
      boost::system::error_code error;
      if (!tls_.ca_file.empty()) {
        context.load_verify_file(tls_.ca_file, error);
      } else {
        context.set_default_verify_paths(error);
      }
      if (error) {
        return make_error(ErrorCode::IoError, "cannot load CA certificates");
      }

      beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
      stream.set_verify_mode(tls_.verify ? ssl::verify_peer : ssl::verify_none);
      if (tls_.verify) {
        stream.set_verify_callback(ssl::host_name_verification(server_name()));
      }
      const std::string name = server_name();
      SSL_ctrl(stream.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
               TLSEXT_NAMETYPE_host_name, const_cast<char*>(name.c_str()));
      beast::get_lowest_layer(stream).connect(endpoints, error);
      if (error) {
        return make_error(ErrorCode::Unavailable, "cannot connect: " + error.message());
      }
      stream.handshake(ssl::stream_base::client, error);
      if (error) {
        return make_error(ErrorCode::Unavailable, "TLS handshake failed: " + error.message());
      }

      auto result = perform_request(stream, std::move(request));
      boost::system::error_code ignored;
      stream.shutdown(ignored);
      return result;
    }

    beast::tcp_stream stream(io_context);
    boost::system::error_code error;
    stream.connect(endpoints, error);
    if (error) {
      return make_error(ErrorCode::Unavailable, "cannot connect: " + error.message());
    }
    auto result = perform_request(stream, std::move(request));
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return result;
  } catch (const std::exception& exception) {
    return make_error(ErrorCode::IoError,
                      std::string("HTTP request failed: ") + exception.what());
  }
}

}  // namespace syncbox::net
