#pragma once

#include <syncbox/core/version_model.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace syncbox::net {

class WsHub;

// One authenticated WebSocket connection. All socket operations are serialized
// on the connection's executor; notify_change() may be called from any thread.
class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(boost::beast::tcp_stream&& stream, WsHub& hub, std::string space_id,
            Seq initial_seq);

  void run(boost::beast::http::request<boost::beast::http::string_body>&& request);

  void send_change(Seq seq);

  [[nodiscard]] const std::string& space_id() const noexcept { return space_id_; }

 private:
  void do_read();
  void handle_message(std::string_view text);
  void enqueue(std::string message);
  void do_write();
  void close();

  boost::beast::websocket::stream<boost::beast::tcp_stream> websocket_;
  WsHub& hub_;
  std::string space_id_;
  Seq initial_seq_;
  boost::beast::flat_buffer buffer_;
  std::deque<std::string> outbox_;
  bool writing_ = false;
  bool closed_ = false;
};

// Tracks live sessions and broadcasts change hints. Hints are advisory only;
// clients still pull changes by cursor (PROTOCOL.md section 3.10).
class WsHub {
 public:
  void add(const std::shared_ptr<WsSession>& session);
  void remove(WsSession* session);
  void notify_change(const std::string& space_id, Seq seq);

  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<WsSession>> sessions_;
};

}  // namespace syncbox::net
