#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace syncbox::fs {

// Gitignore-like subset:
//   *     any characters except '/'
//   **    any characters including '/'
//   ?     one character except '/'
// Patterns without '/' match the basename at any depth. Patterns ending in
// "/**" also match the directory itself, so traversal can stop early.
class IgnoreRules {
 public:
  IgnoreRules() = default;
  explicit IgnoreRules(std::vector<std::string> patterns);

  [[nodiscard]] bool ignored(std::string_view relative_path) const;
  [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }

 private:
  struct Rule {
    std::regex pattern;
    bool basename_only = false;
    std::regex directory_self;  // for "dir/**"; matches "dir"
    bool has_directory_self = false;
  };

  std::vector<Rule> rules_;
};

}  // namespace syncbox::fs
