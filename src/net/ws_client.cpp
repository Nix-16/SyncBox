#include <syncbox/net/ws_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket.hpp>

#include <nlohmann/json.hpp>

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
using asio::ip::tcp;

void parse_url(const std::string& url, std::string& host, std::string& port) {
  std::string_view rest = url;
  constexpr std::string_view kHttp = "http://";
  constexpr std::string_view kWs = "ws://";
  if (rest.rfind(kHttp, 0) == 0) {
    rest.remove_prefix(kHttp.size());
  } else if (rest.rfind(kWs, 0) == 0) {
    rest.remove_prefix(kWs.size());
  }
  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
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

WsClient::WsClient(std::string base_url, std::string space_id, std::string token,
                   ChangeCallback on_change)
    : base_url_(std::move(base_url)), space_id_(std::move(space_id)),
      token_(std::move(token)), on_change_(std::move(on_change)) {
  parse_url(base_url_, host_, port_);
}

WsClient::~WsClient() {
  stop();
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
      websocket::stream<tcp::socket> stream(holder->context);
      boost::system::error_code error;
      tcp::resolver resolver(holder->context);
      const auto endpoints = resolver.resolve(host_, port_, error);
      if (!error) {
        asio::connect(stream.next_layer(), endpoints, error);
      }
      if (!error) {
        stream.set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& request) {
              request.set(http::field::authorization, "Bearer " + token_);
            }));
        stream.handshake(host_, "/v1/ws?space_id=" + space_id_, error);
      }

      if (error) {
        stream.next_layer().close(error);
      } else {
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
                message["type"] == "change" && message.contains("seq")) {
              if (on_change_) {
                on_change_(Seq{message["seq"].get<std::int64_t>()});
              }
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
        stream.next_layer().close(error);
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
