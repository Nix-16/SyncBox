#pragma once

#include <syncbox/core/result.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace syncbox::fs {

enum class WatchEventKind {
  Created,
  Modified,
  Removed,
  Renamed,
};

struct WatchEvent {
  WatchEventKind kind = WatchEventKind::Modified;
  std::string path;                 // relative, '/' separators, UTF-8
  std::optional<std::string> old_path;  // set for Renamed
};

class WatchSink {
 public:
  virtual ~WatchSink() = default;
  virtual void on_event(const WatchEvent& event) = 0;
  virtual void on_error(std::string_view message) = 0;
};

class Watcher {
 public:
  virtual ~Watcher() = default;

  // Stops the background thread. Safe to call more than once.
  virtual void stop() = 0;
};

// Starts a recursive watcher on `root`. Returns I/O error if the platform
// watcher cannot be initialized. Events are delivered on a background thread.
[[nodiscard]] Result<std::unique_ptr<Watcher>> start_watcher(
    const std::filesystem::path& root, WatchSink& sink);

}  // namespace syncbox::fs
