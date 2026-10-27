#pragma once

#include <syncbox/net/http_server.hpp>
#include <syncbox/net/ws_hub.hpp>
#include <syncbox/server/auth_service.hpp>
#include <syncbox/server/blob_store.hpp>
#include <syncbox/server/op_service.hpp>

#include <mutex>

namespace syncbox::server {

// Routes HTTP requests to the auth/op services. Implements PROTOCOL.md
// section 3 for the endpoints available in M4.
class Api final : public net::RequestHandler {
 public:
  Api(ServerStore& store, AuthService& auth, OpService& ops, BlobStore& blobs,
      net::WsHub& hub)
      : store_(store), auth_(auth), ops_(ops), blobs_(blobs), hub_(hub) {}

  [[nodiscard]] net::UpgradeResult handle_upgrade(
      boost::beast::tcp_stream&& stream,
      boost::beast::http::request<boost::beast::http::string_body>&& request,
      const net::HttpRequest& metadata) override;

  [[nodiscard]] net::HttpResponse handle(const net::HttpRequest& request) override;

 private:
  ServerStore& store_;
  AuthService& auth_;
  OpService& ops_;
  BlobStore& blobs_;
  net::WsHub& hub_;
  // SQLite uses a single writer connection; serialize request handling until
  // M9 introduces a dedicated writer queue.
  std::mutex mutex_;
};

}  // namespace syncbox::server
