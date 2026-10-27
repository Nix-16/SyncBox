#pragma once

#include <chrono>
#include <cstdint>

namespace syncbox {

[[nodiscard]] inline std::int64_t now_millis() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace syncbox
