#include <syncbox/core/path.hpp>

#include <utf8proc.h>

#include <array>
#include <cstdlib>
#include <cstring>

namespace syncbox {

namespace {

[[nodiscard]] Result<std::string> utf8proc_map_with_options(std::string_view input,
                                                           utf8proc_option_t options) {
  if (input.find('\0') != std::string_view::npos) {
    return make_error(ErrorCode::PathInvalid, "path contains NUL byte");
  }

  utf8proc_uint8_t* output = nullptr;
  const auto input_length = static_cast<utf8proc_ssize_t>(input.size());
  const auto result = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(input.data()),
                                   input_length, &output, options);
  if (result < 0) {
    return make_error(ErrorCode::PathInvalid, utf8proc_errmsg(result));
  }

  std::string normalized(reinterpret_cast<const char*>(output), static_cast<std::size_t>(result));
  std::free(output);
  return normalized;
}

[[nodiscard]] bool contains_control_character(std::string_view text) {
  const auto* data = reinterpret_cast<const utf8proc_uint8_t*>(text.data());
  const auto length = static_cast<utf8proc_ssize_t>(text.size());
  utf8proc_ssize_t offset = 0;
  while (offset < length) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(data + offset, length - offset, &codepoint);
    if (consumed < 0) {
      return true;
    }
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) {
      return true;
    }
    offset += consumed;
  }
  return false;
}

[[nodiscard]] bool is_ascii_lower(char c) noexcept {
  return c >= 'a' && c <= 'z';
}

[[nodiscard]] bool is_ascii_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] bool equals_ascii_upper(std::string_view text, std::string_view upper) noexcept {
  if (text.size() != upper.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (is_ascii_lower(c)) {
      if (static_cast<char>(c - 'a' + 'A') != upper[i]) {
        return false;
      }
    } else if (c != upper[i]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool has_invalid_windows_character(std::string_view segment) noexcept {
  for (const char c : segment) {
    switch (c) {
      case '<':
      case '>':
      case ':':
      case '"':
      case '|':
      case '?':
      case '*':
      case '\\':
        return true;
      default:
        break;
    }
  }
  return false;
}

[[nodiscard]] Result<void> validate_segment(std::string_view segment) {
  if (segment.empty()) {
    return make_error(ErrorCode::PathInvalid, "empty path segment");
  }
  if (segment == "." || segment == "..") {
    return make_error(ErrorCode::PathInvalid, "dot segment is not allowed");
  }
  if (segment.size() > path_rules::kMaxSegmentBytes) {
    return make_error(ErrorCode::PathInvalid, "path segment exceeds 255 bytes");
  }
  if (has_invalid_windows_character(segment)) {
    return make_error(ErrorCode::PathInvalid, "path segment contains a reserved character");
  }
  if (segment.back() == ' ' || segment.back() == '.') {
    return make_error(ErrorCode::PathInvalid, "path segment ends with space or dot");
  }
  if (is_reserved_windows_name(segment)) {
    return make_error(ErrorCode::PathInvalid, "path segment is a reserved Windows name");
  }
  return Result<void>{};
}

}  // namespace

Result<std::string> normalize_unicode_nfc(std::string_view input) {
  return utf8proc_map_with_options(
      input, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
}

Result<std::string> casefold_unicode(std::string_view input) {
  return utf8proc_map_with_options(
      input, static_cast<utf8proc_option_t>(
                 UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
}

bool is_reserved_windows_name(std::string_view segment) noexcept {
  const auto dot = segment.find('.');
  const std::string_view stem = segment.substr(0, dot);
  if (stem.empty()) {
    return false;
  }

  static constexpr std::array<std::string_view, 4> kFixedNames = {"CON", "PRN", "AUX", "NUL"};
  for (const auto name : kFixedNames) {
    if (equals_ascii_upper(stem, name)) {
      return true;
    }
  }

  if (stem.size() == 4) {
    const bool is_com = equals_ascii_upper(stem.substr(0, 3), "COM");
    const bool is_lpt = equals_ascii_upper(stem.substr(0, 3), "LPT");
    if ((is_com || is_lpt) && is_ascii_digit(stem[3])) {
      return true;
    }
  }
  return false;
}

Result<std::string> normalize_relative_path(std::string_view input) {
  if (input.empty()) {
    return make_error(ErrorCode::PathInvalid, "path is empty");
  }

  auto normalized_result = normalize_unicode_nfc(input);
  if (!normalized_result) {
    return normalized_result.error();
  }
  const std::string& normalized = normalized_result.value();

  if (normalized.front() == '/') {
    return make_error(ErrorCode::PathInvalid, "absolute paths are not allowed");
  }
  if (normalized.back() == '/') {
    return make_error(ErrorCode::PathInvalid, "path must not end with '/'");
  }
  if (normalized.find('\\') != std::string::npos) {
    return make_error(ErrorCode::PathInvalid, "backslash is not allowed");
  }
  if (contains_control_character(normalized)) {
    return make_error(ErrorCode::PathInvalid, "path contains control characters");
  }
  if (normalized.size() > path_rules::kMaxPathBytes) {
    return make_error(ErrorCode::PathInvalid, "path exceeds 1024 bytes");
  }

  std::size_t start = 0;
  while (start <= normalized.size()) {
    const auto slash = normalized.find('/', start);
    const auto end = (slash == std::string::npos) ? normalized.size() : slash;
    const std::string_view segment(normalized.data() + start, end - start);
    auto segment_result = validate_segment(segment);
    if (!segment_result) {
      return segment_result.error();
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }

  return normalized;
}

Result<std::string> make_path_key(std::string_view input) {
  auto normalized_result = normalize_relative_path(input);
  if (!normalized_result) {
    return normalized_result.error();
  }
  return casefold_unicode(normalized_result.value());
}

}  // namespace syncbox
