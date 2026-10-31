#include <syncbox/net/ws_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <functional>
#include <string_view>
#include <utility>

namespace syncbox::net {

class IoContextHolder {
 public:
  boost::asio::io_context context;
};

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using asio::ip::tcp;

void close_lowest(beast::tcp_stream& stream, boost::system::error_code& error) {
  stream.socket().close(error);
}

void close_lowest(tcp::socket& socket, boost::system::error_code& error) {
  socket.close(error);
}

void parse_url(const std::string& url, std::string& host, std::string& port, bool& secure) {
  std::string_view rest = url;
  constexpr std::string_view kHttps = "https://";
  constexpr std::string_view kWss = "wss://";
  constexpr std::string_view kHttp = "http://";
  constexpr std::string_view kWs = "ws://";

  secure = false;
  if (rest.rfind(kHttps, 0) == 0) {
    secure = true;
    rest.remove_prefix(kHttps.size());
  } else if (rest.rfind(kWss, 0) == 0) {
    secure = true;
    rest.remove_prefix(kWss.size());
  } else if (rest.rfind(kHttp, 0) == 0) {
    rest.remove_prefix(kHttp.size());
  } else if (rest.rfind(kWs, 0) == 0) {
    rest.remove_prefix(kWs.size());
  }

  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
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

}  // namespace

WsClient::WsClient(std::string base_url, std::string space_id, std::string token,
                   ChangeCallback on_change, TlsOptions tls)
    : base_url_(std::move(base_url)), space_id_(std::move(space_id)),
      token_(std::move(token)), on_change_(std::move(on_change)), tls_(std::move(tls)) {
  parse_url(base_url_, host_, port_, secure_);
}

WsClient::~WsClient() {
  stop();
}

std::string WsClient::server_name() const {
  return tls_.server_name.empty() ? host_ : tls_.server_name;
}

void WsClient::run() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
    }

    auto holder = std::make_shared<IoContextHolder>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      io_context_ = holder;
      connected_ = false;
    }

    {
      boost::system::error_code error;
      tcp::resolver resolver(holder->context);
      const auto endpoints = resolver.resolve(host_, port_, error);

      auto run_session = [&](auto& stream) {
        stream.set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& request) {
              request.set(http::field::authorization, "Bearer " + token_);
            }));
        stream.handshake(host_, "/v1/ws?space_id=" + space_id_, error);
        if (error) {
          return;
        }

        {
          std::lock_guard<std::mutex> lock(mutex_);
          connected_ = true;
        }
        condition_.notify_all();

        beast::flat_buffer buffer;
        std::function<void()> read = [&] {
          stream.async_read(buffer, [&](boost::system::error_code read_error, std::size_t) {
            if (read_error) {
              holder->context.stop();
              return;
            }
            const std::string text = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            auto message = nlohmann::json::parse(text, nullptr, false);
            if (!message.is_discarded() && message.contains("type") &&
                message["type"] == "change" && message.contains("seq") && on_change_) {
              on_change_(Seq{message["seq"].get<std::int64_t>()});
            }
            read();
          });
        };
        read();
        holder->context.run();

        {
          std::lock_guard<std::mutex> lock(mutex_);
          connected_ = false;
        }
        boost::system::error_code ignored;
        close_lowest(beast::get_lowest_layer(stream), ignored);
      };

      if (!error && secure_) {
        ssl::context context(ssl::context::tls_client);
        if (!tls_.ca_file.empty()) {
          context.load_verify_file(tls_.ca_file, error);
        } else {
          context.set_default_verify_paths(error);
        }
        if (!error) {
          beast::ssl_stream<beast::tcp_stream> tls_stream(holder->context, context);
          tls_stream.set_verify_mode(tls_.verify ? ssl::verify_peer : ssl::verify_none);
          if (tls_.verify) {
            tls_stream.set_verify_callback(ssl::host_name_verification(server_name()));
          }
          const std::string name = server_name();
          SSL_ctrl(tls_stream.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
                   TLSEXT_NAMETYPE_host_name, const_cast<char*>(name.c_str()));
          beast::get_lowest_layer(tls_stream).connect(endpoints, error);
          if (!error) {
            tls_stream.handshake(ssl::stream_base::client, error);
          }
          if (!error) {
            websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream(
                std::move(tls_stream));
            run_session(stream);
          }
        }
      } else if (!error) {
        websocket::stream<tcp::socket> stream(holder->context);
        asio::connect(stream.next_layer(), endpoints, error);
        if (!error) {
          run_session(stream);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (io_context_ == holder) {
        io_context_.reset();
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
    }
    wait_before_reconnect();
  }
}

void WsClient::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    if (io_context_) {
      io_context_->context.stop();
    }
  }
  condition_.notify_all();
}

bool WsClient::wait_connected(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return condition_.wait_for(lock, timeout, [this] { return connected_ || stopping_; }) &&
         connected_;
}

void WsClient::wait_before_reconnect() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; });
}

}  // namespace syncbox::net
