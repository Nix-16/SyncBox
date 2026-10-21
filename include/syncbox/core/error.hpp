#pragma once

#include <string>
#include <string_view>

namespace syncbox {

// Error codes mirror docs/PROTOCOL.md section 1.5. Local-only codes are
// appended after the wire codes and are never sent over the network.
enum class ErrorCode {
  // Wire errors (HTTP API)
  BadRequest,
  AuthRequired,
  TokenExpired,
  DeviceRevoked,
  SpaceForbidden,
  FileNotFound,
  BlobNotFound,
  MethodNotAllowed,
  RevisionConflict,
  PathCaseCollision,
  ResurrectBlocked,
  BlobMissing,
  BlobIncomplete,
  CursorExpired,
  PayloadTooLarge,
  PathInvalid,
  BlobHashMismatch,
  ChunkHashMismatch,
  IdempotencyMismatch,
  InvalidOperation,
  ProtocolUnsupported,
  RateLimited,
  Internal,
  Unavailable,

  // Local errors (client-side only)
  IoError,
  DatabaseError,
  Cancelled,
  Unknown,
};

struct Error {
  ErrorCode code = ErrorCode::Unknown;
  std::string message;
  bool retryable = false;
};

// Wire-format name (e.g. "REVISION_CONFLICT"), stable across releases.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// HTTP status associated with a wire error; 0 for local-only codes.
[[nodiscard]] int http_status(ErrorCode code) noexcept;

// Whether a client should retry after backoff.
[[nodiscard]] bool is_retryable(ErrorCode code) noexcept;

// Builds an Error with retryable derived from the code.
[[nodiscard]] Error make_error(ErrorCode code, std::string message = {});

}  // namespace syncbox
