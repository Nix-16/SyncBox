#include <syncbox/net/ws_hub.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket/error.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

#include <utility>

namespace syncbox::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

}  // namespace

WsSession::WsSession(beast::tcp_stream&& stream, WsHub& hub, std::string space_id,
                     Seq initial_seq)
    : websocket_(std::move(stream)), hub_(hub), space_id_(std::move(space_id)),
      initial_seq_(initial_seq) {}

void WsSession::run(http::request<http::string_body>&& request) {
  websocket_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  websocket_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& response) { response.set(http::field::server, "syncboxd"); }));
  websocket_.read_message_max(64 * 1024);

  auto self = shared_from_this();
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

void WsSession::send_change(Seq seq) {
  auto self = shared_from_this();
  asio::post(websocket_.get_executor(), [self, seq] {
    if (self->closed_) {
      return;
    }
    nlohmann::json message{{"type", "change"}, {"space_id", self->space_id_}, {"seq", seq.value}};
    self->enqueue(message.dump());
  });
}

void WsSession::do_read() {
  auto self = shared_from_this();
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

void WsSession::handle_message(std::string_view text) {
  auto message = nlohmann::json::parse(text, nullptr, false);
  if (message.is_discarded() || !message.contains("type")) {
    return;
  }
  if (message["type"] == "ping") {
    enqueue(nlohmann::json{{"type", "pong"}}.dump());
  }
  // "ack" messages are accepted but the device cursor is updated through
  // the HTTP ack endpoint (M4); WS acks are advisory.
}

void WsSession::enqueue(std::string message) {
  if (closed_) {
    return;
  }
  outbox_.push_back(std::move(message));
  if (!writing_) {
    do_write();
  }
}

void WsSession::do_write() {
  if (outbox_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  auto self = shared_from_this();
  websocket_.async_write(asio::buffer(outbox_.front()),
                         [self](boost::system::error_code error, std::size_t) {
                           if (error) {
                             self->close();
                             return;
                           }
                           self->outbox_.pop_front();
                           self->do_write();
                         });
}

void WsSession::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  hub_.remove(this);
  boost::system::error_code ignored;
  beast::get_lowest_layer(websocket_).socket().close(ignored);
}

void WsHub::add(const std::shared_ptr<WsSession>& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.push_back(session);
}

void WsHub::remove(WsSession* session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(
      std::remove_if(sessions_.begin(), sessions_.end(),
                     [session](const std::weak_ptr<WsSession>& entry) {
                       const auto shared = entry.lock();
                       return !shared || shared.get() == session;
                     }),
      sessions_.end());
}

void WsHub::notify_change(const std::string& space_id, Seq seq) {
  std::vector<std::shared_ptr<WsSession>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto session = it->lock();
      if (!session) {
        it = sessions_.erase(it);
        continue;
      }
      if (session->space_id() == space_id) {
        targets.push_back(std::move(session));
      }
      ++it;
    }
  }
  for (auto& session : targets) {
    session->send_change(seq);
  }
}

std::size_t WsHub::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const auto& entry : sessions_) {
    if (!entry.expired()) {
      ++count;
    }
  }
  return count;
}

}  // namespace syncbox::net
