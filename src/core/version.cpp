#include <syncbox/core/version.hpp>

#ifndef SYNCBOX_VERSION
#define SYNCBOX_VERSION "0.0.0"
#endif

namespace syncbox {

std::string_view version_string() noexcept {
  return SYNCBOX_VERSION;
}

std::string_view project_name() noexcept {
  return "SyncBox";
}

}  // namespace syncbox
