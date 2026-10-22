#include <syncbox/core/version_model.hpp>

#include <cctype>

namespace syncbox {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::string ContentHash::to_hex() const {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::optional<ContentHash> ContentHash::from_hex(std::string_view hex) {
  if (hex.size() != 64) {
    return std::nullopt;
  }

  ContentHash hash;
  for (std::size_t i = 0; i < hash.bytes.size(); ++i) {
    const int high = hex_value(hex[i * 2]);
    const int low = hex_value(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    hash.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return hash;
}

std::string_view to_string(OpKind kind) noexcept {
  switch (kind) {
    case OpKind::Create: return "create";
    case OpKind::Update: return "update";
    case OpKind::Delete: return "delete";
    case OpKind::Rename: return "rename";
    case OpKind::Restore: return "restore";
  }
  return "unknown";
}

std::optional<OpKind> op_kind_from_string(std::string_view text) noexcept {
  if (text == "create") {
    return OpKind::Create;
  }
  if (text == "update") {
    return OpKind::Update;
  }
  if (text == "delete") {
    return OpKind::Delete;
  }
  if (text == "rename") {
    return OpKind::Rename;
  }
  if (text == "restore") {
    return OpKind::Restore;
  }
  return std::nullopt;
}

}  // namespace syncbox
