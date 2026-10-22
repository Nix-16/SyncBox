#include <syncbox/core/uuid.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <random>

namespace syncbox {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] std::uint64_t random_u64() {
  // One generator per thread; seeded from the OS entropy source.
  thread_local std::mt19937_64 engine{std::random_device{}()};
  return engine();
}

}  // namespace

std::string generate_uuid_v7() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

  std::array<std::uint8_t, 16> bytes{};

  // 48-bit big-endian timestamp.
  const std::uint64_t timestamp = millis & 0x0000FFFFFFFFFFFFULL;
  bytes[0] = static_cast<std::uint8_t>((timestamp >> 40U) & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((timestamp >> 32U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((timestamp >> 24U) & 0xFFU);
  bytes[3] = static_cast<std::uint8_t>((timestamp >> 16U) & 0xFFU);
  bytes[4] = static_cast<std::uint8_t>((timestamp >> 8U) & 0xFFU);
  bytes[5] = static_cast<std::uint8_t>(timestamp & 0xFFU);

  const std::uint64_t rand_a = random_u64();
  const std::uint64_t rand_b = random_u64();

  // Version 7 (high nibble) + 12 random bits.
  bytes[6] = static_cast<std::uint8_t>(0x70U | ((rand_a >> 56U) & 0x0FU));
  bytes[7] = static_cast<std::uint8_t>((rand_a >> 48U) & 0xFFU);

  // Variant 0b10 + 62 random bits.
  bytes[8] = static_cast<std::uint8_t>(0x80U | ((rand_b >> 56U) & 0x3FU));
  for (std::size_t i = 9; i < 16; ++i) {
    bytes[i] = static_cast<std::uint8_t>((rand_b >> ((15U - i) * 8U)) & 0xFFU);
  }

  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHexDigits[(bytes[i] >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[bytes[i] & 0x0FU]);
  }
  return out;
}

bool is_uuid(std::string_view text) noexcept {
  if (text.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
      continue;
    }
    const bool is_digit = c >= '0' && c <= '9';
    const bool is_lower = c >= 'a' && c <= 'f';
    const bool is_upper = c >= 'A' && c <= 'F';
    if (!is_digit && !is_lower && !is_upper) {
      return false;
    }
  }
  return true;
}

}  // namespace syncbox
