#pragma once

#include <syncbox/core/version_model.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syncbox::net {

namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
namespace http = boost::beast::http;

class WsHub;

// Type-erased view of a live WebSocket connection, so the hub can hold both
// plaintext and TLS sessions.
class WsConnection {
 public:
  virtual ~WsConnection() = default;
  virtual void send_change(Seq seq) = 0;
  [[nodiscard]] virtual const std::string& space_id() const noexcept = 0;
};

// One authenticated WebSocket connection, generic over the underlying stream
// (beast::tcp_stream for ws://, beast::ssl_stream<tcp_stream> for wss://).
class WsHub {
 public:
  void add(const std::shared_ptr<WsConnection>& session);
  void remove(WsConnection* session);
  void notify_change(const std::string& space_id, Seq seq);

  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<WsConnection>> sessions_;
};

template <typename Stream>
class WsSession final : public WsConnection,
                        public std::enable_shared_from_this<WsSession<Stream>> {
 public:
  WsSession(Stream&& stream, WsHub& hub, std::string space_id, Seq initial_seq)
      : websocket_(std::move(stream)), hub_(hub), space_id_(std::move(space_id)),
        initial_seq_(initial_seq) {}

  void run(http::request<http::string_body>&& request) {
    websocket_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));
    websocket_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response) {
          response.set(http::field::server, "syncboxd");
        }));
    websocket_.read_message_max(64 * 1024);

    auto self = this->shared_from_this();
    websocket_.async_accept(request, [self](boost::system::error_code error) {
      if (error) {
        self->close();
        return;
      }
      nlohmann::json welcome{{"type", "welcome"},
                             {"space_id", self->space_id_},
                             {"seq", self->initial_seq_.value},
                             {"heartbeat_s", 25}};
      self->enqueue(welcome.dump());
      self->do_read();
    });
  }

  void send_change(Seq seq) override {
    auto self = this->shared_from_this();
    boost::asio::post(websocket_.get_executor(), [self, seq] {
      if (self->closed_) {
        return;
      }
      nlohmann::json message{
          {"type", "change"}, {"space_id", self->space_id_}, {"seq", seq.value}};
      self->enqueue(message.dump());
    });
  }

  [[nodiscard]] const std::string& space_id() const noexcept override {
    return space_id_;
  }

 private:
  void do_read() {
    auto self = this->shared_from_this();
    websocket_.async_read(buffer_, [self](boost::system::error_code error, std::size_t) {
      if (error) {
        self->close();
        return;
      }
      const std::string text = beast::buffers_to_string(self->buffer_.data());
      self->buffer_.consume(self->buffer_.size());
      self->handle_message(text);
      self->do_read();
    });
  }

  void handle_message(std::string_view text) {
    auto message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.contains("type")) {
      return;
    }
    if (message["type"] == "ping") {
      enqueue(nlohmann::json{{"type", "pong"}}.dump());
    }
    // "ack" messages are accepted but the device cursor is updated through
    // the HTTP ack endpoint; WS acks are advisory.
  }

  void enqueue(std::string message) {
    if (closed_) {
      return;
    }
    outbox_.push_back(std::move(message));
    if (!writing_) {
      do_write();
    }
  }

  void do_write() {
    if (outbox_.empty()) {
      writing_ = false;
      return;
    }
    writing_ = true;
    auto self = this->shared_from_this();
    websocket_.async_write(
        boost::asio::buffer(outbox_.front()),
        [self](boost::system::error_code error, std::size_t) {
          if (error) {
            self->close();
            return;
          }
          self->outbox_.pop_front();
          self->do_write();
        });
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    hub_.remove(this);
    boost::system::error_code ignored;
    beast::get_lowest_layer(websocket_).socket().close(ignored);
  }

  websocket::stream<Stream> websocket_;
  WsHub& hub_;
  std::string space_id_;
  Seq initial_seq_;
  beast::flat_buffer buffer_;
  std::deque<std::string> outbox_;
  bool writing_ = false;
  bool closed_ = false;
};

}  // namespace syncbox::net
