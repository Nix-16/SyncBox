#pragma once

#include <syncbox/core/error.hpp>

#include <optional>
#include <utility>
#include <variant>

namespace syncbox {

// Minimal Result<T> for C++17. Either holds a value or an Error.
// Library code must not throw across module boundaries; use this type instead.
template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : storage_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return std::get<T>(storage_); }
  const T& value() const& { return std::get<T>(storage_); }
  T&& value() && { return std::get<T>(std::move(storage_)); }

  Error& error() & { return std::get<Error>(storage_); }
  const Error& error() const& { return std::get<Error>(storage_); }

 private:
  std::variant<T, Error> storage_;
};

// Void specialization: success carries no value.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  Error& error() & { return *error_; }
  const Error& error() const& { return *error_; }

 private:
  std::optional<Error> error_;
};

}  // namespace syncbox
