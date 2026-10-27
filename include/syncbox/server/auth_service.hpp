#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/core/version_model.hpp>
#include <syncbox/store/server_store.hpp>

#include <string>
#include <string_view>

namespace syncbox::server {

struct EnrollmentResult {
  std::string device_id;
  std::string device_token;  // plaintext, returned exactly once
  std::string space_id;
  std::string space_name;
  Seq current_seq;
};

class AuthService {
 public:
  explicit AuthService(ServerStore& store) : store_(store) {}

  // SECURITY.md §3.1. Invalid, expired, and already-used enrollment tokens
  // all return AUTH_REQUIRED (no enumeration).
  [[nodiscard]] Result<EnrollmentResult> enroll(std::string_view enrollment_token,
                                                std::string_view device_name,
                                                std::string_view platform,
                                                std::string_view app_version);

  // SECURITY.md §3.2. Returns the authenticated device or AUTH_REQUIRED /
  // DEVICE_REVOKED.
  [[nodiscard]] Result<DeviceRecord> authenticate(std::string_view bearer_token);

  [[nodiscard]] Result<void> revoke(std::string_view device_id, std::string_view reason);

 private:
  ServerStore& store_;
};

}  // namespace syncbox::server
