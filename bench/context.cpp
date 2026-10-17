#include "bench/context.hpp"

#include <mach-o/dyld.h>
#include <sys/sysctl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lob::bench {
namespace {

std::string sysctl_string(const char* name) {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return {};
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return {};
  }
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

}  // namespace

std::string cpu_brand() {
  std::string brand = sysctl_string("machdep.cpu.brand_string");
  if (brand.empty()) {
    brand = sysctl_string("hw.model");
  }
  return brand.empty() ? "unknown" : brand;
}

std::string os_version() {
  const std::string version = sysctl_string("kern.osproductversion");
  return version.empty() ? "macOS unknown" : "macOS " + version;
}

std::string executable_directory() {
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return ".";
  }
  const std::string path(buffer.data());
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace lob::bench
