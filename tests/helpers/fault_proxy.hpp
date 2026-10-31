#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace syncbox::test {

// Minimal HTTP/1.1 forwarding proxy used for fault injection. It can drop the
// connection after the first bytes of the first N blob-download responses so
// the client's Range resume path is exercised with real truncated streams.
class FaultProxy {
 public:
  FaultProxy(std::string upstream_host, std::uint16_t upstream_port)
      : state_(std::make_shared<State>(std::move(upstream_host), upstream_port)) {
    using boost::asio::ip::tcp;
    const tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { accept_loop(); });
  }

  ~FaultProxy() { stop(); }

  FaultProxy(const FaultProxy&) = delete;
  FaultProxy& operator=(const FaultProxy&) = delete;

  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  void interrupt_next_downloads(int count) { state_->interrupts.store(count); }

  [[nodiscard]] int range_requests() const { return state_->range_requests.load(); }

  void stop() {
    if (state_->stopping.exchange(true)) {
      return;
    }
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    io_context_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  struct State {
    State(std::string upstream_host, std::uint16_t upstream_port)
        : host(std::move(upstream_host)), port(upstream_port) {}

    std::string host;
    std::uint16_t port;
    std::atomic<bool> stopping{false};
    std::atomic<int> interrupts{0};
    std::atomic<int> range_requests{0};
  };

  [[nodiscard]] static std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  [[nodiscard]] static std::optional<std::size_t> content_length(const std::string& headers) {
    const std::string lowered = lower(headers);
    constexpr std::string_view key = "\r\ncontent-length:";
    const auto position = lowered.find(key);
    if (position == std::string::npos) {
      return std::nullopt;
    }
    const auto start = position + key.size();
    const auto end = lowered.find("\r\n", start);
    try {
      return static_cast<std::size_t>(std::stoull(headers.substr(start, end - start)));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  static void relay(boost::asio::ip::tcp::socket& source,
                    boost::asio::streambuf& buffered,
                    boost::asio::ip::tcp::socket& destination, std::size_t limit) {
    std::size_t sent = 0;
    if (buffered.size() > 0) {
      const std::size_t take = std::min(limit, buffered.size());
      boost::asio::write(destination, buffered.data(),
                         boost::asio::transfer_exactly(take));
      buffered.consume(take);
      sent += take;
    }

    std::array<char, 16 * 1024> block{};
    while (sent < limit) {
      const std::size_t want = std::min(limit - sent, block.size());
      boost::system::error_code error;
      const std::size_t got = source.read_some(boost::asio::buffer(block.data(), want), error);
      if (error || got == 0) {
        break;
      }
      boost::asio::write(destination, boost::asio::buffer(block.data(), got));
      sent += got;
    }
  }

  static void handle(const std::shared_ptr<State>& state,
                     boost::asio::ip::tcp::socket client) {
    using boost::asio::ip::tcp;
    try {
      boost::asio::io_context context;
      tcp::socket upstream(context);
      upstream.connect({boost::asio::ip::make_address(state->host), state->port});

      boost::asio::streambuf client_buffer;
      boost::asio::read_until(client, client_buffer, "\r\n\r\n");
      const std::string request = boost::beast::buffers_to_string(client_buffer.data());
      client_buffer.consume(request.size());

      const bool blob_get = request.rfind("GET /v1/blobs/", 0) == 0;
      if (lower(request).find("\r\nrange:") != std::string::npos) {
        state->range_requests.fetch_add(1);
      }
      boost::asio::write(upstream, boost::asio::buffer(request));

      boost::asio::streambuf upstream_buffer;
      boost::asio::read_until(upstream, upstream_buffer, "\r\n\r\n");
      const std::string response = boost::beast::buffers_to_string(upstream_buffer.data());
      upstream_buffer.consume(response.size());
      boost::asio::write(client, boost::asio::buffer(response));

      const auto length = content_length(response);
      if (!length.has_value()) {
        return;
      }

      bool interrupt = false;
      if (blob_get) {
        int remaining = state->interrupts.load();
        while (remaining > 0) {
          if (state->interrupts.compare_exchange_weak(remaining, remaining - 1)) {
            interrupt = true;
            break;
          }
        }
      }

      if (interrupt) {
        // Deliver only a prefix, then drop both sides.
        relay(upstream, upstream_buffer, client, std::min<std::size_t>(4096, *length));
      } else {
        relay(upstream, upstream_buffer, client, *length);
      }

      boost::system::error_code ignored;
      client.shutdown(tcp::socket::shutdown_both, ignored);
      client.close(ignored);
      upstream.close(ignored);
    } catch (const std::exception&) {
      // Fault injection closes sockets mid-stream; errors are expected.
    }
  }

  void do_accept() {
    using boost::asio::ip::tcp;
    acceptor_.async_accept([this](boost::system::error_code error, tcp::socket client) {
      if (!error && client.is_open()) {
        std::thread([state = state_, socket = std::move(client)]() mutable {
          // Re-home the accepted socket onto a context owned by this thread;
          // otherwise the proxy's io_context may be destroyed while the
          // handler is still using the socket.
          try {
            boost::asio::io_context local;
            tcp::socket detached(local);
            detached.assign(tcp::v4(), socket.release());
            handle(state, std::move(detached));
          } catch (const std::exception&) {
          }
        }).detach();
      }
      if (!state_->stopping.load() && !error) {
        do_accept();
      }
    });
  }

  void accept_loop() {
    do_accept();
    io_context_.run();
  }

  std::shared_ptr<State> state_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_{io_context_};
  std::uint16_t port_ = 0;
  std::thread thread_;
};

}  // namespace syncbox::test
