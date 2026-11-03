#pragma once

#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_hub.hpp>
#include <syncbox/server/auth_service.hpp>
#include <syncbox/server/blob_store.hpp>
#include <syncbox/server/op_service.hpp>
#include <syncbox/server/rate_limiter.hpp>

#include <mutex>

namespace syncbox::server {

// Routes HTTP requests to the auth/op services. Implements PROTOCOL.md
// section 3 for the endpoints available in M4.
class Api final : public net::RequestHandler {
 public:
  Api(ServerStore& store, AuthService& auth, OpService& ops, BlobStore& blobs,
      net::WsHub& hub, RateLimitConfig limits = {}, std::string admin_token = {})
      : store_(store), auth_(auth), ops_(ops), blobs_(blobs), hub_(hub),
        rate_limits_(limits), admin_token_(std::move(admin_token)) {}

  // Replaces the per-device limits and clears bucket state (tests/ops).
  void set_rate_limits(RateLimitConfig limits);

  [[nodiscard]] net::UpgradeResult handle_upgrade(
      boost::beast::tcp_stream&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const net::HttpRequest& metadata) override;

  [[nodiscard]] net::UpgradeResult handle_upgrade(
      boost::beast::ssl_stream<boost::beast::tcp_stream>&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const net::HttpRequest& metadata) override;

  [[nodiscard]] net::HttpResponse handle(const net::HttpRequest& request) override;

 private:
  struct UpgradeContext {
    std::string space_id;
    std::string device_id;
    Seq seq;
  };

  [[nodiscard]] Result<UpgradeContext> validate_upgrade(const net::HttpRequest& metadata);

  ServerStore& store_;
  AuthService& auth_;
  OpService& ops_;
  BlobStore& blobs_;
  net::WsHub& hub_;
  // SQLite uses a single writer connection; serialize request handling until
  // M9 introduces a dedicated writer queue.
  std::mutex mutex_;
  RateLimitConfig rate_limits_;
  RateLimiter rate_limiter_;
  // When non-empty, DELETE /v1/devices/{id} requires this bearer token.
  std::string admin_token_;
};

}  // namespace syncbox::server
