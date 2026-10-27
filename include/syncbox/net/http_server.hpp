#pragma once

#include <syncbox/core/result.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace syncbox::net {

struct HttpRequest {
  boost::beast::http::verb method = boost::beast::http::verb::get;
  std::string target;
  std::string body;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
  unsigned status = 200;
  std::string body;
  std::string content_type = "application/json; charset=utf-8";
  std::vector<std::pair<std::string, std::string>> headers;
};

struct UpgradeResult {
  bool upgraded = false;
  HttpResponse rejection;
};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual HttpResponse handle(const HttpRequest& request) = 0;

  // Called for WebSocket upgrade requests to /v1/ws. Returning
  // upgraded=true transfers ownership of the stream; otherwise rejection is
  // sent as a normal HTTP response.
  virtual UpgradeResult handle_upgrade(
      boost::beast::tcp_stream&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const HttpRequest& metadata) {
    (void)stream;
    (void)request;
    (void)metadata;
    return UpgradeResult{};
  }
};

// Minimal asynchronous HTTP/1.1 server. Handlers run on the server's
// io_context threads and must be thread-safe (the API layer serializes
// database writes).
class HttpServer {
 public:
  HttpServer(std::string address, std::uint16_t port, RequestHandler& handler,
             std::size_t thread_count = 2);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  [[nodiscard]] Result<void> start();
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }

 private:
  void do_accept();

  std::string address_;
  std::uint16_t bound_port_;
  RequestHandler& handler_;
  std::size_t thread_count_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
};

}  // namespace syncbox::net
