#pragma once

#include <syncbox/net/http_server.hpp>
#include <syncbox/server/auth_service.hpp>
#include <syncbox/server/op_service.hpp>

#include <mutex>

namespace syncbox::server {

// Routes HTTP requests to the auth/op services. Implements PROTOCOL.md
// section 3 for the endpoints available in M4.
class Api final : public net::RequestHandler {
 public:
  Api(ServerStore& store, AuthService& auth, OpService& ops)
      : store_(store), auth_(auth), ops_(ops) {}

  [[nodiscard]] net::HttpResponse handle(const net::HttpRequest& request) override;

 private:
  ServerStore& store_;
  AuthService& auth_;
  OpService& ops_;
  // SQLite uses a single writer connection; serialize request handling until
  // M9 introduces a dedicated writer queue.
  std::mutex mutex_;
};

}  // namespace syncbox::server
