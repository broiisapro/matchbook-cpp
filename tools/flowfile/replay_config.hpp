#pragma once

#include "lob/config.hpp"

namespace lob::flowfile {

/// Configuration shared by the flow tools (gen, replay, difftest) so a
/// generated flow and a replay of it agree on price range and capacities
/// without extra flags. Overridable per invocation on the command line.
inline lob::Config default_replay_config() { return lob::Config{1, 200000, 1048576, 200000}; }

}  // namespace lob::flowfile
