#include <syncbox/server/auth_service.hpp>

#include <syncbox/common/time.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>

#include <array>
#include <vector>

namespace syncbox::server {

namespace {

[[nodiscard]] bool valid_platform(std::string_view platform) noexcept {
  return platform == "linux" || platform == "windows" || platform == "macos";
}

[[nodiscard]] std::vector<std::uint8_t> to_bytes(const ContentHash& hash) {
  return std::vector<std::uint8_t>(hash.bytes.begin(), hash.bytes.end());
}

}  // namespace

Result<EnrollmentResult> AuthService::enroll(std::string_view enrollment_token,
                                             std::string_view device_name,
                                             std::string_view platform,
                                             std::string_view app_version) {
  if (!valid_platform(platform)) {
    return make_error(ErrorCode::BadRequest, "unsupported platform");
  }
  if (device_name.empty()) {
    return make_error(ErrorCode::BadRequest, "device_name is required");
  }

  auto enrollment_hash = hash_token(enrollment_token);
  if (!enrollment_hash) {
    return enrollment_hash.error();
  }
  const std::vector<std::uint8_t> enrollment_bytes = to_bytes(enrollment_hash.value());

  auto token_record = store_.find_enrollment_token(enrollment_bytes);
  if (!token_record) {
    return token_record.error();
  }
  if (!token_record.value().has_value()) {
    return make_error(ErrorCode::AuthRequired, "invalid enrollment token");
  }
  const EnrollmentTokenRecord& record = *token_record.value();
  const std::int64_t now = now_millis();
  if (record.used_at.has_value() || record.expires_at <= now) {
    return make_error(ErrorCode::AuthRequired, "invalid enrollment token");
  }

  auto device_token = generate_token();
  if (!device_token) {
    return device_token.error();
  }
  auto device_token_hash = hash_token(device_token.value());
  if (!device_token_hash) {
    return device_token_hash.error();
  }

  DeviceRecord device;
  device.device_id = generate_uuid_v7();
  device.space_id = record.space_id;
  device.display_name = std::string(device_name);
  device.platform = std::string(platform);
  device.token_hash = to_bytes(device_token_hash.value());
  device.app_version = std::string(app_version);
  device.created_at = now;
  device.last_seen_at = now;
  device.last_ack_seq = Seq{0};

  auto transaction = Transaction::begin(store_.db());
  if (!transaction) {
    return transaction.error();
  }

  auto inserted = store_.insert_device(device);
  if (!inserted) {
    return inserted.error();
  }
  auto marked = store_.mark_enrollment_token_used(enrollment_bytes, device.device_id, now);
  if (!marked) {
    return marked.error();
  }
  auto committed = transaction.value().commit();
  if (!committed) {
    return committed.error();
  }

  auto space = store_.find_space(record.space_id);
  if (!space) {
    return space.error();
  }
  auto latest = store_.latest_seq(record.space_id);
  if (!latest) {
    return latest.error();
  }

  EnrollmentResult result;
  result.device_id = device.device_id;
  result.device_token = device_token.value();
  result.space_id = record.space_id;
  result.space_name = space.value().has_value() ? space.value()->name : std::string{};
  result.current_seq = latest.value();
  return result;
}

Result<DeviceRecord> AuthService::authenticate(std::string_view bearer_token) {
  auto token_hash = hash_token(bearer_token);
  if (!token_hash) {
    return token_hash.error();
  }
  const std::vector<std::uint8_t> bytes = to_bytes(token_hash.value());

  auto device = store_.find_device_by_token_hash(bytes);
  if (!device) {
    return device.error();
  }
  if (!device.value().has_value()) {
    return make_error(ErrorCode::AuthRequired, "invalid device token");
  }

  const DeviceRecord& record = *device.value();
  if (record.token_hash.size() != bytes.size() ||
      !constant_time_equal(record.token_hash.data(), bytes.data(), bytes.size())) {
    return make_error(ErrorCode::AuthRequired, "invalid device token");
  }
  if (record.status != "active") {
    return make_error(ErrorCode::DeviceRevoked, "device has been revoked");
  }
  return record;
}

Result<void> AuthService::revoke(std::string_view device_id, std::string_view reason) {
  return store_.set_device_status(device_id, "revoked", std::string(reason));
}

}  // namespace syncbox::server
