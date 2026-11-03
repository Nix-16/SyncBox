#include <gtest/gtest.h>

#include <syncbox/net/http_server.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <string>

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

class OkHandler final : public syncbox::net::RequestHandler {
 public:
  syncbox::net::HttpResponse handle(const syncbox::net::HttpRequest&) override {
    syncbox::net::HttpResponse response;
    response.status = 200;
    response.body = R"({"status":"ok"})";
    return response;
  }
};

}  // namespace

TEST(HttpServerTest, ClosesSlowlorisConnectionAfterIdleTimeout) {
  OkHandler handler;
  syncbox::net::HttpServer server("127.0.0.1", 0, handler, 1);
  server.set_idle_timeout(std::chrono::milliseconds(200));
  ASSERT_TRUE(server.start());

  asio::io_context context;
  tcp::socket socket(context);
  socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});

  // Send headers without the terminating blank line.
  const std::string partial = "GET /v1/health HTTP/1.1\r\nHost: test\r\n";
  asio::write(socket, asio::buffer(partial));

  bool deadline_fired = false;
  bool closed_by_server = false;
  asio::steady_timer deadline(context);
  socket.async_wait(tcp::socket::wait_read,
                    [&](boost::system::error_code /*error*/) {
                      // EOF and RST both mean the peer closed the connection.
                      closed_by_server = !deadline_fired;
                      deadline.cancel();
                    });
  deadline.expires_after(std::chrono::seconds(2));
  deadline.async_wait([&](boost::system::error_code error) {
    if (!error) {
      deadline_fired = true;
      boost::system::error_code ignored;
      socket.close(ignored);
    }
  });

  context.run();

  EXPECT_TRUE(closed_by_server) << "server did not close an idle partial request";
  boost::system::error_code ignored;
  socket.close(ignored);
  server.stop();
}
