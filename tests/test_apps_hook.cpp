#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

/// The tests run from the build tree, so walk up until the source root
/// (the directory that holds CMakeLists.txt and tests/) is found.
fs::path find_source_root() {
  fs::path directory = fs::current_path();
  for (int depth = 0; depth < 6; ++depth) {
    if (fs::exists(directory / "CMakeLists.txt") && fs::is_directory(directory / "tests")) {
      return directory;
    }
    if (!directory.has_parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return {};
}

std::optional<std::string> cache_value(std::string_view key) {
  fs::path directory = fs::current_path();
  for (int depth = 0; depth < 6; ++depth) {
    const fs::path cache = directory / "CMakeCache.txt";
    if (fs::exists(cache)) {
      std::ifstream input(cache);
      std::string line;
      while (std::getline(input, line)) {
        if (line.rfind(key, 0) != 0 || line.size() <= key.size()) {
          continue;
        }
        const char separator = line[key.size()];
        if (separator == ':' || separator == '=') {
          const std::size_t value = line.find('=', key.size());
          if (value == std::string::npos) {
            return std::nullopt;
          }
          return line.substr(value + 1);
        }
      }
      return std::nullopt;
    }
    if (!directory.has_parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return std::nullopt;
}

/// Decision C52: the root CMakeLists.txt adds apps/<name> for every directory
/// under apps/ that has a CMakeLists.txt. With no apps/ directory in the tree
/// the hook has to add nothing and leave the configure untouched.
TEST(AppsHook, EmptyAppsDirectoryIsANoOp) {
  const fs::path root = find_source_root();
  ASSERT_FALSE(root.empty()) << "source root not found from " << fs::current_path();

  const fs::path apps = root / "apps";
  if (fs::exists(apps) && fs::is_directory(apps)) {
    for (const fs::directory_entry& entry : fs::directory_iterator(apps)) {
      if (entry.is_directory() && fs::exists(entry.path() / "CMakeLists.txt")) {
        GTEST_SKIP() << "the tree has a real app at " << entry.path().string();
      }
    }
  }

  const std::optional<std::string> configured = cache_value("LOB_APP_SUBDIRECTORIES");
  ASSERT_TRUE(configured.has_value()) << "the hook did not record anything in CMakeCache.txt";
  EXPECT_TRUE(configured->empty()) << "the hook added app directories: " << *configured;
}

}  // namespace
