# Epic 02 — the array price ladder

## Goal

Epic 1 left the fast engine with a `std::map` price-level container: correct,
allocation-free after construction thanks to the node pool, but every level
lookup walked a red-black tree and every best-price query started at
`begin()`. Epic 2 replaces that container with a flat array indexed by price
plus a two-level occupancy bitmap, proves the two containers produce identical
events, makes the array version the default, and measures the before/after
difference on an idle machine. The tree stays in the repository as the
baseline the comparison is made against.

## How it works

### The ladder

`include/lob/array_ladder.hpp` defines `ArrayPriceLadder`, one instance per
side of the book:

- **Flat level array.** The array covers every tick in
  `[min_price, max_price]`, so a level is addressed by `price - min_price` with
  no comparisons and no hashing. A slot holds the same `Level` struct as the
  tree ladder (FIFO head and tail as 32-bit pool indices, `total_qty`,
  `order_count`), so the intrusive order queues and the matching code are
  unchanged.
- **Two-level occupancy bitmap.** One bit per tick records whether a level
  exists, packed into 64-bit words. A second bitmap has one bit per word and is
  set exactly when that word is non-zero, so the search for the next occupied
  level skips 64 empty words at a time instead of testing them one by one.
  Asks scan up with `std::countr_zero`; bids scan down with
  `std::countl_zero`. Bits are set and cleared in the same two functions that
  create and remove levels (`find_or_insert` and `erase_if_empty`), so no
  second copy of the occupancy state can go stale.
- **Best-price tracking.** Each side remembers its best index. A new level
  that beats the current best updates it in O(1); when the best level empties,
  the next best is recomputed from the bitmap, which is the only case that
  scans. Best-price queries are therefore a load of the tracked index, not a
  tree descent.
- **Bounded range.** The range is limited to 2^24 ticks per side (16,777,216
  slots). Construction throws `std::invalid_argument` for an inverted range or
  a wider one, so an unsupported configuration fails at book construction
  instead of over-allocating. With `Level` at 24 bytes the hard ceiling is
  about 384 MB per side; a normal configuration is far smaller (65,536 ticks
  is about 1.5 MB per side). Memory scales with the configured price range,
  not with the number of live levels.
- **Allocation once.** The level array, the words and the summary bitmap are
  three `std::unique_ptr<T[]>` allocations in the constructor. The hot path
  (add, cancel, modify, match) allocates nothing, which `test_alloc` proves on
  the array build as well as the tree build.

The array ladder exposes the same operations as the tree ladder
(`find`, `find_or_insert`, `erase_if_empty`, `best`, `best_price`,
`level_count`, `max_levels`, `empty`, `for_each_best_first`), with the same
per-side `max_levels` limit and the same best-first iteration order. The engine
picks one at compile time through a private alias in `order_book.hpp`; no
public API mentions the ladder.

### Build option and presets

`LOB_PRICE_LADDER` is a CMake cache option (`array` by default, `tree` for the
baseline). It selects the ladder type and defines `LOB_PRICE_LADDER_ARRAY` or
`LOB_PRICE_LADDER_TREE` publicly, because `OrderBook`'s layout depends on the
choice and every consumer has to compile against the same one. Every configure
preset pins the option explicitly (decision C37), so a persistent build
directory can never silently keep an old value:

- `release`, `release-array`, `debug`, `asan`, `tsan` → array ladder;
- `release-tree` → tree ladder, the before side of the comparison.

Both ladders build every target and pass the full test suite and the
differential test; CI tests the default (array), and the nightly and the
fresh-clone audits also test the tree build.

## Key decisions and reasoning

- **The tree stayed as a baseline instead of being deleted.** A before/after
  claim needs both sides buildable from the same source at the same commit,
  and keeping the tree exercises the ladder seam. It costs one header, one
  build option and one preset.
- **The seam is a class with a matching interface, not a virtual base.**
  The ladder sits on the hot path; a virtual call per level lookup would tax
  the design it is meant to speed up. The type is chosen once at compile time
  and the matching code is written against the common method set.
- **Two bitmap levels, not one.** A single bitset would need a linear word scan
  between occupied prices. The summary word makes the search two dependent
  `countr_zero`/`countl_zero` operations in the common case and a short scan
  of summary words in the sparse case.
- **Best price is tracked, not searched.** The common operation is "what is
  the best price now"; tracking it turns that into one load. The trade-off is
  a recomputation when the best level empties, which happens at most once per
  level per time it becomes best.
- **A hard range limit at 2^24 ticks.** The array trades memory for access
  time, so an unbounded range would turn a configuration mistake into a
  multi-gigabyte allocation. 2^24 ticks is far beyond the instrument ranges
  the project targets (the replay default spans 200,000 ticks) and keeps the
  worst case under 384 MB per side.
- **Out-of-range prices are unaddressable.** The engine validates prices
  against `Config` before a level is created, so the ladder treats an
  out-of-range lookup as "no level" instead of converting it into an index.
  This hardens the ladder without changing engine semantics.
- **The array ladder became the default only after hardening.** The first
  landing (#56) added the implementation behind the option; a second change
  (#58) added edge coverage, the invariant checks over the bitmap and the
  differential runs on both ladders before switching the default and CI to
  array. The tree build stayed in the nightly and the audit.
- **The published comparison is a single interleaved run.** Epic 1's numbers
  were never published; Epic 2's are, and they come from one
  `scripts/run_benchmarks.sh --compare` invocation on an otherwise idle
  machine. The script alternates tree/array rounds so thermal drift hits both
  sides.

## Interfaces for future epics

- **`PriceLadder` seam.** Any future price-level container implements the
  method set above and the `Level` layout, then joins the compile-time choice
  in `order_book.hpp`. Matching, the digest walk, the invariant checker and the
  differential harness do not change. The array header includes the tree
  header for `Level` and `kNullIndex`, and `types.hpp` for `Side`; it pulls in
  no other engine header.
- **Presets.** `release` (array), `release-array` (explicit array) and
  `release-tree` (tree) give later epics reproducible builds of either side.
  New presets inherit `base` and pin `LOB_PRICE_LADDER`.
- **`scripts/run_benchmarks.sh --compare`.** The only sanctioned producer of
  before/after numbers: it builds both presets, runs `lob_bench --self-test`,
  interleaves two rounds per build, merges the rounds and renders
  `build/bench-compare.md`. Single-build mode remains the way to produce a
  latency/throughput report for one build.
- **`scripts/bench_report.py --before/--after`.** Renders the comparison table
  from two Google Benchmark JSON files (shared environment, p50/p99/p99.9 with
  after/before ratios, MixedFlow throughput). The same renderer produces the
  single-build latency table (p50/p90/p99/p99.9/max and throughput), so later
  epics can publish either view without a new script.

## Measured before/after

From one idle-machine run of `scripts/run_benchmarks.sh --compare` on
2026-10-28, commit `aed8e66`, `bench-compare-wave03.md`. Both builds are
Apple M5, macOS 27.0.1, Apple clang 21.0.0 (clang-2100.3.34.2), `-O3 -DNDEBUG`,
timer `cntvct_el0` at 1 GHz with a 41.0 ns step, `batch128` latency mode, 10
repetitions per benchmark (two rounds of five, interleaved). Latency in
nanoseconds, median run per build:

| Benchmark | p50 tree | p50 array | p50 ratio | p99 tree | p99 array | p99 ratio | p99.9 tree | p99.9 array | p99.9 ratio |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Add order, non-crossing | 14 | 13 | 0.93× | 23 | 22 | 0.96× | 24 | 24 | 1.00× |
| Add order, crossing 1 level | 9 | 8 | 0.89× | 10 | 9 | 0.90× | 10 | 10 | 1.00× |
| Add order, crossing 2 levels | 15 | 14 | 0.93× | 17 | 16 | 0.94× | 21 | 19 | 0.90× |
| Add order, crossing 3 levels | 20 | 19 | 0.95× | 23 | 22 | 0.96× | 27 | 23 | 0.85× |
| Cancel, resting | 27 | 26 | 0.96× | 35 | 35 | 1.00× | 65 | 65 | 1.00× |
| Market order, sweep 1 level | 9 | 8 | 0.89× | 10 | 9 | 0.90× | 11 | 10 | 0.91× |
| Market order, sweep 2 levels | 15 | 14 | 0.93× | 17 | 16 | 0.94× | 20 | 18 | 0.90× |
| Market order, sweep 3 levels | 20 | 19 | 0.95× | 23 | 22 | 0.96× | 25 | 23 | 0.92× |
| Mixed flow | 33 | 32 | 0.97× | 47 | 46 | 0.98× | 84 | 80 | 0.95× |

| Benchmark | tree ops/s | array ops/s | ratio (array/tree) |
| --- | --- | --- | --- |
| Mixed flow | 29,515,306 | 30,470,598 | 1.03× |

Ratios are after/before, so below 1.00 means the array build is faster. The
gain is concentrated where a match walks levels: crossing and market orders
take 4–11% off p50 (0.89–0.95×), and the three-level sweep's p99.9 drops from
27 ns to 23 ns (0.85×). Cancels are 0.96× at p50 and unchanged at p99, and
non-crossing adds are unchanged at p99.9. That is a few percent overall, not
an order of magnitude: the tree was already allocation-free and its depth was
small in this workload. `batch128` samples average 128 operations, so the
p99/p99.9 columns smooth the tail of any single operation; they compare the
two builds, not the worst individual add.

## Profiling evidence

`scripts/profile_ladder.sh` records a `Time Profiler` trace around
`lob_bench --benchmark_filter=MixedFlow` for the array build (`build/profile-array`)
and the tree build (`build/profile-tree`), exports the `time-profile` table and
prints the top 15 symbols by self time side by side. One sample is one
millisecond of on-CPU time, and the trace covers the whole process, so the
one-time prologue and Google Benchmark's own setup are included in the shares.
These are profile shares from a busy machine, not latency measurements.

`./scripts/profile_ladder.sh` (5 s minimum time per build, 2026-10-28, commit
`aed8e66`) produced 14,737 tree samples and 7,488 array samples. The
ladder-relevant rows:

| Symbol | tree self% | array self% |
| --- | --- | --- |
| `MixedFlowFixture::step` (fixture) | 21.06% | 18.74% |
| `IdMap::erase` | 11.56% | 11.20% |
| `IdMap::find` | 10.51% | 10.87% |
| `OrderBook::unlink` | 9.58% | 11.70% |
| `PriceOrder::operator()` | 3.91% | — |
| `std::__tree::__find_equal` (node lookup) | 2.37% | — |
| Array ladder index/occupancy helpers | — | below the top 15 |

The tree build spends 6.28% of its samples in the map's comparator and
lookup helper, both of which disappear from the array build's top 15; the
array build's price-container work is inlined into `find_or_insert` and
`erase_if_empty` and sits at or below the 0.95% top-15 cutoff. Everything else
in the profile — FIFO unlinking, id-map probing, the fixture's own step
function, Google Benchmark's CPU detection, the random generator — is shared
code and keeps roughly the same share in both builds. The sample counts also
differ because the array build finishes the same benchmark sooner. This is
consistent with the measured change: strict improvements on the operations
that walk levels, flat results where the ladder was not the cost.

## Deviations

- The architecture document's memory example (2 sides × 65,536 ticks × 32 B
  ≈ 4 MB) used a 32-byte `Level`; the implemented `Level` is 24 bytes. The
  2^24-tick hard limit bounds the worst case at about 384 MB per side.
- `release-array` was added as an explicit preset alongside `release` so the
  array side of a comparison can never be confused with a default that might
  change again (decision C37).
- The array ladder became the default in #58, before Epic 1 was formally
  closed (Epic 1 close was itself deferred until the timer carryover #57
  merged and was re-verified). The ladder work and the timer fix therefore
  landed in the same window, and the published numbers use the post-#57
  harness (`batch128`, measured granularity).
- `PriceLadder` (the tree) and `ArrayPriceLadder` are two concrete classes
  with the same method set; there is no abstract base class. The write-up in
  Epic 1 described "the same `PriceLadder` interface", which is the seam as
  used by callers, not a C++ virtual interface.

## Known limitations

- **Range is bounded by the array.** A book wider than 2^24 ticks cannot use
  the array ladder; the tree ladder remains available for such a
  configuration, and the array constructor refuses loudly rather than
  under-allocating.
- **Memory scales with the range.** A 2^24-tick book allocates about 384 MB
  per side for levels alone, plus the two bitmaps. This is the expected
  trade-off, but it is a real constraint for many instruments in one process
  (out of scope: this engine is single-instrument).
- **The before/after gain is small in absolute terms.** The tree baseline was
  already allocation-free and shallow, so the array ladder buys a few
  nanoseconds per operation, not a step change. The larger win is that
  best-price and next-level operations no longer depend on tree depth, which
  matters more for deep books than for the benchmark's shallow fixtures.
- **Percentiles are batch averages.** On this machine the counter steps by
  about 41 ns, so a single operation cannot be timed; `batch128` reports the
  per-op average over 128 operations and smooths the tail by that factor.
  True per-operation tails would need a finer clock.
- **macOS has no pinning.** The QoS hint is the only scheduling control, so
  the tails include scheduler noise even on an idle machine; the numbers
  belong to the machine and flags above.
