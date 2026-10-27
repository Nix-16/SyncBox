#include <syncbox/net/http_server.hpp>

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace syncbox::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(asio::ip::tcp::socket socket, RequestHandler& handler)
      : stream_(std::move(socket)), handler_(handler) {}

  void run() { do_read(); }

 private:
  void do_read() {
    parser_.emplace();
    parser_->body_limit(kMaxBodyBytes);

    http::async_read(stream_, buffer_, *parser_,
                     [self = shared_from_this()](boost::system::error_code error,
                                                 std::size_t) {
                       if (error == http::error::end_of_stream) {
                         self->do_close();
                         return;
                       }
                       if (error) {
                         return;
                       }
                       self->handle_request();
                     });
  }

  void handle_request() {
    auto request = parser_->release();
    buffer_.consume(buffer_.size());

    const unsigned version = request.version();
    const bool keep_alive = request.keep_alive();
    const std::string target(request.target());

    HttpRequest input;
    input.method = request.method();
    input.target = target;
    input.body = request.body();
    input.content_type = std::string(request[http::field::content_type]);
    for (const auto& field : request) {
      input.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
    }

    if (beast::websocket::is_upgrade(request) && target.rfind("/v1/ws", 0) == 0) {
      auto decision = handler_.handle_upgrade(std::move(stream_), std::move(request), input);
      if (decision.upgraded) {
        return;
      }
      if (decision.rejection.status == 0) {
        decision.rejection.status = 400;
        decision.rejection.body =
            R"({"error":{"code":"BAD_REQUEST","message":"websocket upgrade failed"}})";
      }
      send_response(std::move(decision.rejection), version, false);
      return;
    }

    HttpResponse output;
    try {
      output = handler_.handle(input);
    } catch (const std::exception& exception) {
      output.status = 500;
      output.body =
          std::string(R"({"error":{"code":"INTERNAL","message":")") + exception.what() + "\"}}";
    } catch (...) {
      output.status = 500;
      output.body = R"({"error":{"code":"INTERNAL","message":"unhandled exception"}})";
    }

    send_response(std::move(output), version, keep_alive);
  }

  void send_response(HttpResponse output, unsigned version, bool keep_alive) {
    auto response = std::make_shared<http::response<http::string_body>>(
        static_cast<http::status>(output.status), version);
    response->set(http::field::content_type, output.content_type);
    response->set(http::field::server, "syncboxd");
    for (const auto& header : output.headers) {
      response->set(header.first, header.second);
    }
    response->body() = std::move(output.body);
    response->keep_alive(keep_alive);
    response->prepare_payload();

    http::async_write(
        stream_, *response,
        [self = shared_from_this(), response](boost::system::error_code error, std::size_t) {
          if (error) {
            self->do_close();
            return;
          }
          if (!response->keep_alive()) {
            self->do_close();
            return;
          }
          self->do_read();
        });
  }

  void do_close() {
    boost::system::error_code ignored;
    beast::get_lowest_layer(stream_).socket().shutdown(asio::ip::tcp::socket::shutdown_send,
                                                       ignored);
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  RequestHandler& handler_;
};

}  // namespace

HttpServer::HttpServer(std::string address, std::uint16_t port, RequestHandler& handler,
                       std::size_t thread_count)
    : address_(std::move(address)),
      bound_port_(port),
      handler_(handler),
      thread_count_(thread_count),
      acceptor_(io_context_) {}

HttpServer::~HttpServer() {
  stop();
}

Result<void> HttpServer::start() {
  boost::system::error_code error;
  const auto address = asio::ip::make_address(address_, error);
  if (error) {
    return make_error(ErrorCode::BadRequest, "invalid listen address: " + address_);
  }

  const asio::ip::tcp::endpoint endpoint(address, bound_port_);
  acceptor_.open(endpoint.protocol(), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot open listener: " + error.message());
  }
  acceptor_.set_option(asio::socket_base::reuse_address(true), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot set SO_REUSEADDR: " + error.message());
  }
  acceptor_.bind(endpoint, error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot bind listener: " + error.message());
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot listen: " + error.message());
  }

  bound_port_ = acceptor_.local_endpoint(error).port();
  if (error) {
    return make_error(ErrorCode::IoError, "cannot read local endpoint: " + error.message());
  }

  do_accept();
  for (std::size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this] { io_context_.run(); });
  }
  return Result<void>{};
}

void HttpServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  io_context_.stop();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

void HttpServer::do_accept() {
  acceptor_.async_accept(io_context_, [this](boost::system::error_code error,
                                             asio::ip::tcp::socket socket) {
    if (!error) {
      std::make_shared<Session>(std::move(socket), handler_)->run();
    }
    if (!stopping_.load()) {
      do_accept();
    }
  });
}

}  // namespace syncbox::net
