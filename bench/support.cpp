#include "bench/support.hpp"

#include <atomic>

namespace lob::bench {
namespace {

std::atomic<int> g_harness_failures{0};

}  // namespace

int harness_failures() { return g_harness_failures.load(); }

void note_harness_failure() { g_harness_failures.fetch_add(1); }

}  // namespace lob::bench
