#pragma once

#include <cstdlib>
#include <string_view>

namespace syncbox::fault {

// Synthetic ENOSPC injection for fault-injection tests (TEST_PLAN FI-09/10).
// The environment is read on every call so tests can toggle it at runtime.

[[nodiscard]] inline bool enabled(std::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

[[nodiscard]] inline bool client_disk_full() {
  return enabled("SYNCBOX_FAULT_CLIENT_DISK_FULL");
}

[[nodiscard]] inline bool server_disk_full() {
  return enabled("SYNCBOX_FAULT_SERVER_DISK_FULL");
}

}  // namespace syncbox::fault
