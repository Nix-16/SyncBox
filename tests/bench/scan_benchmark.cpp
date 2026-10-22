// Manual benchmark: creates N files in a temporary directory and measures
// scan_directory() time. Not part of CTest.
#include <syncbox/fs/ignore_rules.hpp>
#include <syncbox/fs/scanner.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  int count = 10000;
  if (argc >= 2) {
    count = std::atoi(argv[1]);
    if (count <= 0) {
      std::cerr << "count must be positive\n";
      return 2;
    }
  }

  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("syncbox-scan-bench-" + std::to_string(unique));
  std::filesystem::create_directories(root);

  const int directories = 100;
  for (int d = 0; d < directories; ++d) {
    std::filesystem::create_directories(root / ("dir" + std::to_string(d)));
  }
  for (int i = 0; i < count; ++i) {
    const auto path =
        root / ("dir" + std::to_string(i % directories)) / ("file" + std::to_string(i) + ".txt");
    std::ofstream stream(path, std::ios::binary);
    stream << "x";
  }

  const auto start = std::chrono::steady_clock::now();
  auto result = syncbox::fs::scan_directory(root, syncbox::fs::IgnoreRules{});
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  if (!result) {
    std::cerr << "scan failed: " << result.error().message << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }

  std::cout << "files=" << result.value().entries.size() << " elapsed_ms=" << elapsed
            << " errors=" << result.value().errors.size() << '\n';

  std::filesystem::remove_all(root);
  return 0;
}
