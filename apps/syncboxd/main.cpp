#include <syncbox/core/version.hpp>

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "Usage: syncboxd [--version | --help]\n"
    "SyncBox server (M1 skeleton; HTTP API not implemented yet).\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) {
    std::cout << kUsage;
    return 0;
  }

  if (argc == 2) {
    const std::string_view arg{argv[1]};
    if (arg == "--version" || arg == "-v") {
      std::cout << syncbox::project_name() << " server " << syncbox::version_string() << '\n';
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return 0;
    }
    std::cerr << "syncboxd: unknown argument: " << arg << '\n' << kUsage;
    return 2;
  }

  std::cerr << "syncboxd: too many arguments\n" << kUsage;
  return 2;
}
