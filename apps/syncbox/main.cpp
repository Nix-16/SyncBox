#include <syncbox/core/version.hpp>

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "Usage: syncbox [--version | --help]\n"
    "SyncBox client (M1 skeleton; synchronization engine not implemented yet).\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) {
    std::cout << kUsage;
    return 0;
  }

  if (argc == 2) {
    const std::string_view arg{argv[1]};
    if (arg == "--version" || arg == "-v") {
      std::cout << syncbox::project_name() << " " << syncbox::version_string() << '\n';
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return 0;
    }
    std::cerr << "syncbox: unknown argument: " << arg << '\n' << kUsage;
    return 2;
  }

  std::cerr << "syncbox: too many arguments\n" << kUsage;
  return 2;
}
