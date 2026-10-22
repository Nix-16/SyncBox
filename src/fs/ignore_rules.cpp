#include <syncbox/fs/ignore_rules.hpp>

#include <algorithm>
#include <string>

namespace syncbox::fs {

namespace {

[[nodiscard]] bool is_regex_special(char c) noexcept {
  switch (c) {
    case '.':
    case '^':
    case '$':
    case '|':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '+':
    case '\\':
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::string glob_to_regex(std::string_view glob) {
  std::string out = "^";
  for (std::size_t i = 0; i < glob.size(); ++i) {
    const char c = glob[i];
    if (c == '*') {
      if (i + 1 < glob.size() && glob[i + 1] == '*') {
        if (i + 2 < glob.size() && glob[i + 2] == '/') {
          // "**/" matches zero or more directories.
          out += "(?:.*/)?";
          i += 2;
        } else {
          out += ".*";
          ++i;
        }
      } else {
        out += "[^/]*";
      }
    } else if (c == '?') {
      out += "[^/]";
    } else {
      if (is_regex_special(c)) {
        out.push_back('\\');
      }
      out.push_back(c);
    }
  }
  out += "$";
  return out;
}

[[nodiscard]] std::string normalize_pattern(std::string_view pattern) {
  std::string result(pattern);
  while (result.size() > 2 && result[0] == '.' && result[1] == '/') {
    result.erase(0, 2);
  }
  if (!result.empty() && result.front() == '/') {
    result.erase(0, 1);
  }
  return result;
}

[[nodiscard]] std::string_view basename(std::string_view path) {
  const auto slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

}  // namespace

IgnoreRules::IgnoreRules(std::vector<std::string> patterns) {
  for (auto& raw : patterns) {
    std::string pattern = normalize_pattern(raw);
    if (pattern.empty()) {
      continue;
    }

    Rule rule;
    rule.basename_only = pattern.find('/') == std::string::npos;

    constexpr std::string_view kDirectorySuffix = "/**";
    if (pattern.size() > kDirectorySuffix.size() &&
        pattern.compare(pattern.size() - kDirectorySuffix.size(), kDirectorySuffix.size(),
                        kDirectorySuffix) == 0) {
      const std::string directory = pattern.substr(0, pattern.size() - kDirectorySuffix.size());
      rule.directory_self = std::regex(glob_to_regex(directory));
      rule.has_directory_self = true;
    }

    rule.pattern = std::regex(glob_to_regex(pattern));
    rules_.push_back(std::move(rule));
  }
}

bool IgnoreRules::ignored(std::string_view relative_path) const {
  if (relative_path.empty()) {
    return false;
  }
  while (!relative_path.empty() && relative_path.front() == '/') {
    relative_path.remove_prefix(1);
  }

  const std::string path(relative_path);
  const std::string_view name = basename(relative_path);

  for (const auto& rule : rules_) {
    const bool matched =
        rule.basename_only
            ? std::regex_match(name.begin(), name.end(), rule.pattern)
            : std::regex_match(path, rule.pattern);
    if (matched) {
      return true;
    }
    if (rule.has_directory_self && std::regex_match(path, rule.directory_self)) {
      return true;
    }
  }
  return false;
}

}  // namespace syncbox::fs
