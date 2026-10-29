#include <syncbox/net/http_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/optional.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace syncbox::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

void parse_url(const std::string& url, std::string& host, std::string& port,
               std::string& prefix) {
  std::string_view rest = url;
  constexpr std::string_view kScheme = "http://";
  if (rest.rfind(kScheme, 0) == 0) {
    rest.remove_prefix(kScheme.size());
  }
  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
  prefix = slash == std::string_view::npos ? std::string{} : std::string(rest.substr(slash));

  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    host = std::string(authority);
    port = "80";
  } else {
    host = std::string(authority.substr(0, colon));
    port = std::string(authority.substr(colon + 1));
  }
  if (host.empty()) {
    host = "127.0.0.1";
  }
}

}  // namespace

HttpClient::HttpClient(std::string base_url) : base_url_(std::move(base_url)) {
  parse_url(base_url_, host_, port_, prefix_);
}

Result<ClientResponse> HttpClient::request(
    http::verb method, std::string_view target, std::string_view body, std::string_view token,
    std::vector<std::pair<std::string, std::string>> headers) {
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(host_, port_);

    beast::tcp_stream stream(io_context);
    stream.connect(endpoints);

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

    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return result;
  } catch (const std::exception& exception) {
    return make_error(ErrorCode::IoError,
                      std::string("HTTP request failed: ") + exception.what());
  }
}

Result<unsigned> HttpClient::download_to_file(std::string_view target, std::string_view token,
                                              const std::filesystem::path& destination) {
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(host_, port_);
    beast::tcp_stream stream(io_context);
    stream.connect(endpoints);

    http::request<http::empty_body> request{http::verb::get, std::string(target), 11};
    request.set(http::field::host, host_);
    if (!token.empty()) {
      request.set(http::field::authorization, "Bearer " + std::string(token));
    }
    request.prepare_payload();
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
    const unsigned status = parser.get().result_int();

    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return status;
  } catch (const std::exception& exception) {
    return make_error(ErrorCode::IoError,
                      std::string("HTTP download failed: ") + exception.what());
  }
}

}  // namespace syncbox::net
