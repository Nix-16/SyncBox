#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace syncbox {

// Stable identity of a file, assigned by the server on create.
// Remains unchanged across rename, delete, and restore.
struct FileId {
  std::string value;

  [[nodiscard]] bool empty() const noexcept { return value.empty(); }

  friend bool operator==(const FileId& lhs, const FileId& rhs) { return lhs.value == rhs.value; }
  friend bool operator!=(const FileId& lhs, const FileId& rhs) { return !(lhs == rhs); }
  friend bool operator<(const FileId& lhs, const FileId& rhs) { return lhs.value < rhs.value; }
};

// Client-generated UUIDv7 identifying one logical operation (idempotency key).
struct OpId {
  std::string value;

  [[nodiscard]] bool empty() const noexcept { return value.empty(); }

  friend bool operator==(const OpId& lhs, const OpId& rhs) { return lhs.value == rhs.value; }
  friend bool operator!=(const OpId& lhs, const OpId& rhs) { return !(lhs == rhs); }
};

// Monotonic per-file version number. 0 means "does not exist yet".
struct Revision {
  std::int64_t value = 0;

  [[nodiscard]] static constexpr Revision none() noexcept { return Revision{0}; }
  [[nodiscard]] bool exists() const noexcept { return value > 0; }
  [[nodiscard]] Revision next() const noexcept { return Revision{value + 1}; }

  friend bool operator==(Revision lhs, Revision rhs) { return lhs.value == rhs.value; }
  friend bool operator!=(Revision lhs, Revision rhs) { return !(lhs == rhs); }
  friend bool operator<(Revision lhs, Revision rhs) { return lhs.value < rhs.value; }
  friend bool operator<=(Revision lhs, Revision rhs) { return lhs.value <= rhs.value; }
  friend bool operator>(Revision lhs, Revision rhs) { return rhs < lhs; }
  friend bool operator>=(Revision lhs, Revision rhs) { return rhs <= lhs; }
};

// Server-global monotonic change sequence used as the sync cursor.
struct Seq {
  std::int64_t value = 0;

  friend bool operator==(Seq lhs, Seq rhs) { return lhs.value == rhs.value; }
  friend bool operator!=(Seq lhs, Seq rhs) { return !(lhs == rhs); }
  friend bool operator<(Seq lhs, Seq rhs) { return lhs.value < rhs.value; }
  friend bool operator<=(Seq lhs, Seq rhs) { return lhs.value <= rhs.value; }
  friend bool operator>(Seq lhs, Seq rhs) { return rhs < lhs; }
  friend bool operator>=(Seq lhs, Seq rhs) { return rhs <= lhs; }
};

// SHA-256 digest of file content. All-zero is a valid digest, so presence is
// expressed with std::optional<ContentHash> where "unknown" is possible.
struct ContentHash {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static std::optional<ContentHash> from_hex(std::string_view hex);

  friend bool operator==(const ContentHash& lhs, const ContentHash& rhs) { return lhs.bytes == rhs.bytes; }
  friend bool operator!=(const ContentHash& lhs, const ContentHash& rhs) { return !(lhs == rhs); }
  friend bool operator<(const ContentHash& lhs, const ContentHash& rhs) { return lhs.bytes < rhs.bytes; }
};

enum class OpKind {
  Create,
  Update,
  Delete,
  Rename,
  Restore,
};

[[nodiscard]] std::string_view to_string(OpKind kind) noexcept;
[[nodiscard]] std::optional<OpKind> op_kind_from_string(std::string_view text) noexcept;

// Materialized current state of one entry (server `entries` row projection).
struct EntryState {
  FileId file_id;
  Revision revision;
  std::string path;
  std::optional<ContentHash> content_hash;
  std::int64_t size = 0;
  bool deleted = false;
  Seq updated_seq;
};

}  // namespace syncbox
