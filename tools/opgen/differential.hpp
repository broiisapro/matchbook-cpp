#pragma once

// Differential harness: runs one operation stream into two engines, records
// every listener event with a recording listener per engine, and after each
// operation requires the two event vectors to be equal and the two digests to
// match. The first disagreement is captured in full; later disagreements are
// only counted, so a run always consumes the requested number of operations.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "tests/support/recording_listener.hpp"
#include "tools/opgen/opgen.hpp"

namespace lob::opgen {

/// Applies one generated operation to an engine with the OrderBook API.
template <class Engine>
void apply_op(Engine& engine, const Op& op) {
  switch (op.kind) {
    case OpKind::Submit:
      engine.submit(op.id, op.side, op.type, op.price, op.qty);
      break;
    case OpKind::Cancel:
      engine.cancel(op.id);
      break;
    case OpKind::Modify:
      engine.modify(op.id, op.price, op.qty);
      break;
  }
}

/// Outcome of a differential run. The op/events/digest fields describe the
/// first mismatching operation (index zero based); they are only meaningful
/// when mismatches > 0. events_differ and digest_differ say which comparison
/// failed first. The depth fields are the deepest number of price levels seen
/// on each side of either engine, sampled every kDepthSampleInterval
/// operations and after the last one.
struct DiffResult {
  std::uint64_t ops_run = 0;
  std::uint64_t mismatches = 0;
  std::uint64_t first_mismatch_index = 0;
  bool events_differ = false;
  bool digest_differ = false;
  Op op{};
  std::vector<lob::test::Event> events_a;
  std::vector<lob::test::Event> events_b;
  std::uint64_t digest_a = 0;
  std::uint64_t digest_b = 0;
  std::uint64_t max_bid_depth_a = 0;
  std::uint64_t max_ask_depth_a = 0;
  std::uint64_t max_bid_depth_b = 0;
  std::uint64_t max_ask_depth_b = 0;
};

/// Calls the progress callback after this many operations.
inline constexpr std::uint64_t kProgressInterval = 100000;

/// Samples book depth after this many operations (and after the final one).
/// Sampling keeps the per-side level walk out of every-iteration cost while
/// still seeing the deepest books of a run.
inline constexpr std::uint64_t kDepthSampleInterval = 100;

/// Records the current number of levels per side of one engine into the
/// matching maxima. `out` must hold at least `config.max_levels` entries so
/// the count is exact.
template <class Engine>
void sample_depth(const Engine& engine, std::vector<lob::LevelInfo>& out, std::uint64_t& max_bid,
                  std::uint64_t& max_ask) {
  max_bid = std::max(max_bid, static_cast<std::uint64_t>(engine.top_levels(lob::Side::Buy, out)));
  max_ask = std::max(max_ask, static_cast<std::uint64_t>(engine.top_levels(lob::Side::Sell, out)));
}

/// Runs `ops` generated operations through `EngineA` and `EngineB`, both
/// constructed with `config`. After every operation: the two recording
/// listener event vectors must be identical, then the two digests must be
/// equal. Mismatching operations are counted and the first one is captured in
/// full; the run still consumes every requested operation. `progress`, when
/// set, is called with the number of completed operations every
/// kProgressInterval operations. Book depth is sampled every
/// kDepthSampleInterval operations.
template <class EngineA, class EngineB>
DiffResult run_differential(const Config& config, OpGenerator& generator, std::uint64_t ops,
                            std::function<void(std::uint64_t)> progress = {}) {
  lob::test::RecordingListener listener_a;
  lob::test::RecordingListener listener_b;
  EngineA engine_a(config, listener_a);
  EngineB engine_b(config, listener_b);
  std::vector<lob::LevelInfo> depth_buffer(config.max_levels);

  DiffResult result;
  for (std::uint64_t index = 0; index < ops; ++index) {
    const Op op = generator.next();
    listener_a.clear();
    listener_b.clear();

    apply_op(engine_a, op);
    apply_op(engine_b, op);

    const bool events_equal = listener_a.events == listener_b.events;
    const std::uint64_t digest_a = engine_a.digest();
    const std::uint64_t digest_b = engine_b.digest();

    if (!events_equal || digest_a != digest_b) {
      if (result.mismatches == 0) {
        result.first_mismatch_index = index;
        result.events_differ = !events_equal;
        result.digest_differ = digest_a != digest_b;
        result.op = op;
        result.events_a = listener_a.events;
        result.events_b = listener_b.events;
        result.digest_a = digest_a;
        result.digest_b = digest_b;
      }
      ++result.mismatches;
    }

    result.ops_run = index + 1;
    if ((index + 1) % kDepthSampleInterval == 0 || index + 1 == ops) {
      sample_depth(engine_a, depth_buffer, result.max_bid_depth_a, result.max_ask_depth_a);
      sample_depth(engine_b, depth_buffer, result.max_bid_depth_b, result.max_ask_depth_b);
    }
    if (progress && (index + 1) % kProgressInterval == 0) {
      progress(index + 1);
    }
  }
  return result;
}

}  // namespace lob::opgen
