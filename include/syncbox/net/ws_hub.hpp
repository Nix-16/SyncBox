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
    std::lock_guard<std::mutex> lock(mutex_);
    websocket_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));
    websocket_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response) {
          response.set(http::field::server, "syncboxd");
        }));
    websocket_.read_message_max(64 * 1024);

    auto self = this->shared_from_this();
    websocket_.async_accept(request, [self](boost::system::error_code error) {
      std::lock_guard<std::mutex> handler_lock(self->mutex_);
      if (error) {
        self->close_locked();
        return;
      }
      nlohmann::json welcome{{"type", "welcome"},
                             {"space_id", self->space_id_},
                             {"seq", self->initial_seq_.value},
                             {"heartbeat_s", 25}};
      // Changes may have been queued before the handshake completed; the
      // welcome message must still be the first frame written.
      self->ready_ = true;
      self->outbox_.push_front(welcome.dump());
      if (!self->writing_) {
        self->do_write_locked();
      }
      self->do_read_locked();
    });
  }

  void send_change(Seq seq) override {
    auto self = this->shared_from_this();
    boost::asio::post(websocket_.get_executor(), [self, seq] {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (self->closed_) {
        return;
      }
      nlohmann::json message{
          {"type", "change"}, {"space_id", self->space_id_}, {"seq", seq.value}};
      self->enqueue_locked(message.dump());
    });
  }

  [[nodiscard]] const std::string& space_id() const noexcept override {
    return space_id_;
  }

 private:
  void do_read_locked() {
    auto self = this->shared_from_this();
    websocket_.async_read(buffer_, [self](boost::system::error_code error, std::size_t) {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (error) {
        self->close_locked();
        return;
      }
      const std::string text = beast::buffers_to_string(self->buffer_.data());
      self->buffer_.consume(self->buffer_.size());
      self->handle_message(text);
      self->do_read_locked();
    });
  }

  void handle_message(std::string_view text) {
    auto message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.contains("type")) {
      return;
    }
    if (message["type"] == "ping") {
      enqueue_locked(nlohmann::json{{"type", "pong"}}.dump());
    }
    // "ack" messages are accepted but the device cursor is updated through
    // the HTTP ack endpoint; WS acks are advisory.
  }

  void enqueue_locked(std::string message) {
    if (closed_) {
      return;
    }
    outbox_.push_back(std::move(message));
    // Never initiate a write before the server-side handshake completed.
    if (ready_ && !writing_) {
      do_write_locked();
    }
  }

  void do_write_locked() {
    if (outbox_.empty()) {
      writing_ = false;
      if (close_after_write_) {
        close_socket_locked();
      }
      return;
    }
    writing_ = true;
    auto self = this->shared_from_this();
    websocket_.async_write(
        boost::asio::buffer(outbox_.front()),
        [self](boost::system::error_code error, std::size_t) {
          std::lock_guard<std::mutex> lock(self->mutex_);
          self->writing_ = false;
          if (error) {
            self->outbox_.clear();
            if (!self->closed_) {
              self->closed_ = true;
              self->hub_.remove(self.get());
            }
            self->close_socket_locked();
            return;
          }
          self->outbox_.pop_front();
          if (self->close_after_write_) {
            self->close_socket_locked();
            return;
          }
          self->do_write_locked();
        });
  }

  void close_locked() {
    if (closed_) {
      return;
    }
    closed_ = true;
    hub_.remove(this);
    // A pending write still owns the stream; let its completion handler close
    // the socket so the stream is not destroyed with an outstanding op.
    if (writing_) {
      close_after_write_ = true;
      return;
    }
    close_socket_locked();
  }

  void close_socket_locked() {
    closed_ = true;
    close_after_write_ = false;
    boost::system::error_code ignored;
    beast::get_lowest_layer(websocket_).socket().close(ignored);
  }

  std::mutex mutex_;
  websocket::stream<Stream> websocket_;
  WsHub& hub_;
  std::string space_id_;
  Seq initial_seq_;
  beast::flat_buffer buffer_;
  std::deque<std::string> outbox_;
  bool writing_ = false;
  bool closed_ = false;
  bool close_after_write_ = false;
  bool ready_ = false;
};

}  // namespace syncbox::net
