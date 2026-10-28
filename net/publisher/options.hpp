#pragma once

// Command-line parsing for lob_publisher. Kept in the library so the parser is
// covered by the test binary; the executable is only main().

#include <string>
#include <string_view>

#include "net/publisher/publisher.hpp"

namespace lob::publisher {

struct PublisherOptions {
  PublisherConfig config;
  bool help = false;
};

std::string_view publisher_usage();

/// Parses argv[1..]. Returns false and fills `error` on a malformed or unknown
/// option. `--help` succeeds with options.help set.
bool parse_publisher_options(int argc, char** argv, PublisherOptions& options, std::string& error);

}  // namespace lob::publisher
