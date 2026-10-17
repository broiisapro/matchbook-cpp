#pragma once

#include <string>

namespace lob::bench {

/// CPU brand from `sysctl machdep.cpu.brand_string` ("Apple M5"), falling back
/// to `hw.model`, then "unknown".
std::string cpu_brand();

/// macOS product version from `sysctl kern.osproductversion`, prefixed with
/// "macOS".
std::string os_version();

/// Directory containing the running executable; used for the default
/// benchmark output next to the binary.
std::string executable_directory();

}  // namespace lob::bench
