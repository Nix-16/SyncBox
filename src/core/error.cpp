#include <syncbox/core/error.hpp>

#include <utility>

namespace syncbox {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::BadRequest: return "BAD_REQUEST";
    case ErrorCode::AuthRequired: return "AUTH_REQUIRED";
    case ErrorCode::TokenExpired: return "TOKEN_EXPIRED";
    case ErrorCode::DeviceRevoked: return "DEVICE_REVOKED";
    case ErrorCode::SpaceForbidden: return "SPACE_FORBIDDEN";
    case ErrorCode::FileNotFound: return "FILE_NOT_FOUND";
    case ErrorCode::BlobNotFound: return "BLOB_NOT_FOUND";
    case ErrorCode::MethodNotAllowed: return "METHOD_NOT_ALLOWED";
    case ErrorCode::RevisionConflict: return "REVISION_CONFLICT";
    case ErrorCode::PathCaseCollision: return "PATH_CASE_COLLISION";
    case ErrorCode::ResurrectBlocked: return "RESURRECT_BLOCKED";
    case ErrorCode::BlobMissing: return "BLOB_MISSING";
    case ErrorCode::BlobIncomplete: return "BLOB_INCOMPLETE";
    case ErrorCode::CursorExpired: return "CURSOR_EXPIRED";
    case ErrorCode::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
    case ErrorCode::PathInvalid: return "PATH_INVALID";
    case ErrorCode::BlobHashMismatch: return "BLOB_HASH_MISMATCH";
    case ErrorCode::ChunkHashMismatch: return "CHUNK_HASH_MISMATCH";
    case ErrorCode::IdempotencyMismatch: return "IDEMPOTENCY_MISMATCH";
    case ErrorCode::InvalidOperation: return "INVALID_OPERATION";
    case ErrorCode::ProtocolUnsupported: return "PROTOCOL_UNSUPPORTED";
    case ErrorCode::RateLimited: return "RATE_LIMITED";
    case ErrorCode::Internal: return "INTERNAL";
    case ErrorCode::Unavailable: return "UNAVAILABLE";
    case ErrorCode::IoError: return "IO_ERROR";
    case ErrorCode::DatabaseError: return "DATABASE_ERROR";
    case ErrorCode::Cancelled: return "CANCELLED";
    case ErrorCode::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

int http_status(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::BadRequest: return 400;
    case ErrorCode::AuthRequired:
    case ErrorCode::TokenExpired: return 401;
    case ErrorCode::DeviceRevoked:
    case ErrorCode::SpaceForbidden: return 403;
    case ErrorCode::FileNotFound:
    case ErrorCode::BlobNotFound: return 404;
    case ErrorCode::MethodNotAllowed: return 405;
    case ErrorCode::RevisionConflict:
    case ErrorCode::PathCaseCollision:
    case ErrorCode::ResurrectBlocked:
    case ErrorCode::BlobMissing: return 409;
    case ErrorCode::CursorExpired: return 410;
    case ErrorCode::PayloadTooLarge: return 413;
    case ErrorCode::BlobIncomplete: return 409;
    case ErrorCode::PathInvalid:
    case ErrorCode::BlobHashMismatch:
    case ErrorCode::ChunkHashMismatch:
    case ErrorCode::IdempotencyMismatch:
    case ErrorCode::InvalidOperation: return 422;
    case ErrorCode::ProtocolUnsupported: return 426;
    case ErrorCode::RateLimited: return 429;
    case ErrorCode::Internal: return 500;
    case ErrorCode::Unavailable: return 503;
    case ErrorCode::IoError:
    case ErrorCode::DatabaseError:
    case ErrorCode::Cancelled:
    case ErrorCode::Unknown: return 0;
  }
  return 0;
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::BlobIncomplete:
    case ErrorCode::ChunkHashMismatch:
    case ErrorCode::RateLimited:
    case ErrorCode::Internal:
    case ErrorCode::Unavailable:
    case ErrorCode::IoError:
      return true;
    default:
      return false;
  }
}

Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message), is_retryable(code)};
}

}  // namespace syncbox
