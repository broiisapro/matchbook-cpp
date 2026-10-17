# Epic 01 — engines, differential harness, tools, benchmarks

## Goal

Epic 1 turns the frozen interfaces of Epic 0 into two working engines and the
machinery that proves they agree. The fast engine is the allocation-free hot
path the whole project is about; the reference engine is a deliberately naive
implementation of the same event contract that acts as the arbiter of
correctness. Around them this epic lands the seeded operation generator, the
differential harness and its `lobsim difftest` front end, the flow-file tools
with a committed Tardis-derived sample, the benchmark suites with an HDR
latency harness, and the scripts that turn benchmark JSON into a report.

No benchmark numbers are published here. Publishable numbers come from
`scripts/run_benchmarks.sh` on an otherwise idle machine; this document describes
the method and the JSON contract only.

## How it works

### Fast engine

The fast engine lives behind the frozen `lob::OrderBook` API and does no heap
allocation on the hot path after construction. Its pieces:

- **Order pool** (`include/lob/pool.hpp`): fixed capacity `max_orders`, one
  contiguous slab allocated in the constructor, 32-bit indices, a LIFO free
  list threaded through the unused slots, and `kNull` returned when the pool is
  exhausted. Slots are a union of the order node and the free-list link, so
  the slab is exactly sized and aligned without constructing nodes up front.
  The same header provides `PoolAllocator<T>`, a stateful, rebind-able node
  allocator for `std::map`; every copy shares one control block, and the
  control block's node size is discovered by inserting into a scratch map with
  a recording allocator rather than hard-coding libc++'s tree-node layout.
- **Intrusive FIFO** (`include/lob/intrusive_list.hpp`): an O(1),
  allocation-free queue view over pool indices. The queue endpoints are owned
  by the price level and the links live inside the order nodes.
- **Id map** (`include/lob/id_map.hpp`): open addressing from `OrderId` to pool
  index, capacity `next power of two >= 2 * max_orders`, linear probing,
  backward-shift deletion (no tombstones), never rehashes, and every 64-bit id
  including 0 and `UINT64_MAX` is a valid key (a separate occupancy array
  avoids a sentinel problem). The hash is a 64-bit mixer (splitmix64-style
  finalizer).
- **Price ladder, Epic 1 baseline** (`include/lob/price_ladder.hpp`): one
  `std::map<Price, Level, PriceOrder, PoolAllocator>` per side, bids ordered
  descending and asks ascending so `begin()` is the best level and iteration
  is best → worst. A `Level` holds head/tail pool indices plus `total_qty` and
  `order_count`. `find_or_insert` refuses a new level when the side already
  holds `max_levels` levels. The map is hidden behind this class so Epic 2's
  array-indexed ladder can replace it without touching callers.
- **Matching** (PR #11, extended by #12): a taker is validated in the contract
  order (quantity, price for limit/IOC, duplicate live id), emitted through
  `on_accept`, and then matched against the best opposite level. The best
  level is re-read at the top of every iteration, each fill emits
  `on_trade(maker, taker, maker_price, fill)` then
  `on_book_update(opposite, maker_price, new level qty)`, a fully filled maker
  is unlinked silently, and a partially filled maker stays at the head of its
  queue. Limit remainders rest under the capacity rules; market and IOC
  remainders are cancelled. `modify` reduces in place (keeping queue position)
  or does cancel + new, exactly per the event contract.
- **Queries and digest**: `best_bid`, `best_ask`, `level_qty`, `top_levels`
  (best first, truncated), `order_count`, and `digest()`, which walks the
  levels best → worst per side and streams ids and remaining quantities into
  the shared `DigestBuilder`.
- **Discipline machinery**: all callbacks go through private `notify_*`
  helpers guarded by a reentrancy flag that every public entry asserts in
  debug builds (PR #53). A test-only friend (`OrderBookTestAccess`, defined in
  the invariants test file, never shipped) checks after every operation that
  the book is not crossed, that level aggregates match their FIFOs, that the
  FIFO links are consistent and cycle-free, that the id map and live count
  agree, and that the free-list size plus live orders equals the pool
  capacity. A separate `test_alloc` binary replaces all eight `operator new`
  and twelve `operator delete` forms and proves zero allocations across
  1,000,000 warm operations; the test skips under sanitizers because the
  interceptors make counts unreliable on macOS.

### Reference engine

`lob::ref::ReferenceBook` is written from the event contract in
`docs/architecture.md` §4, never from the fast engine (decision C19 and the
independence the differential test depends on). It uses
`std::map<Price, std::list<Order>>` per side (bids `std::greater`) plus
`std::unordered_map<OrderId, Location>` for the id index, all allocation
allowed, no optimization on purpose. It shares only the frozen headers and may
include `lob/order_book.hpp` for the `LevelInfo` struct (decision C23). All
limits of the contract are enforced by counting: `max_orders` live orders and
`max_levels` levels per side. Its tests pin hand-written event vectors for
every classic-bug scenario (modify reduce keeps priority, modify increase
loses it, partial fill keeps queue position, trade at the resting price,
market against an empty book, unknown-id cancel, duplicate ids, IOC,
sweeps, capacity cancels, reject precedence, digest against a hand-built
stream).

### Operation generator

`tools/opgen/opgen.hpp`/`.cpp` produces a deterministic `Op` stream from a
seed. Randomness is `std::mt19937_64` only; every distribution is hand-written
with integer arithmetic (rejection-sampled uniform integers, integer
comparisons for bernoulli decisions, cumulative integer weights for choices),
so there is no `std::*_distribution` and no floating point and a seed produces
the same stream on every standard library and in every build type. The mix is
weighted toward the places engines disagree: crossing orders, modifies at the
best level, cancels of queue heads and tails, IOC and market against possibly
empty sides, duplicate and unknown ids, ids reused after a cancel, prices at
the configured edges and just outside, and invalid quantities. The generator
keeps its own view of the orders it has submitted (as submitted, not as
filled) and random-walks a mid price so the modelled book has depth. A
committed XXH64 golden of the first 10,000 serialized operations freezes the
default stream.

### Differential harness and `lobsim difftest`

`tools/opgen/differential.hpp` runs one generated stream into two engines,
records each engine's events with a `RecordingListener`, and after every
operation requires the two event vectors to be identical and the two digests
to be equal. The first mismatch is captured in full (index, op, both event
vectors, both digests); later mismatches are counted while the run still
consumes all requested operations. `tests/test_differential_harness.cpp`
proves the harness catches three injected faults at the exact op index: a
dropped `on_book_update`, a trade price shifted by one tick, and a
digest-only queue-position difference; it also runs the reference engine
against itself for one million operations with zero mismatches.

`tools/lobsim/difftest.cpp` wraps the harness in the CLI:

```
lobsim difftest --ops N --seed S [--engine-a fast|reference]
                [--engine-b fast|reference]
                [--min-price P --max-price P --max-orders N --max-levels N]
                [--progress]
```

Defaults are fast vs reference with a deliberately small config
(`{1, 500, 512, 16}`) so a default run hits capacity cancels for orders and
levels and both ends of the price range; every field can be overridden.
Progress lines go to stderr; stdout ends with a `key: value` block containing
`ops`, `seed`, `mismatches` and `wall_ms`. Exit status is 0 for no mismatch, 1
for a mismatch and 2 for a usage error. A mismatch prints the failing op,
both event vectors and digests, and a repro shrunk by greedily deleting
operations from the failing prefix (back to front, 30-second time box),
printed as pasteable `lob::opgen::Op` aggregate initializers.

`tests/test_differential.cpp` and `tests/test_cross_engine.cpp` hold the
arbiter at scale: `OrderBook` vs `ReferenceBook` for one million operations
plus three stress configs (tight order capacity, tight level capacity, narrow
price range) at 200k operations, and two seeded flows where the full trade
lists and the final digests must be identical. Under sanitizers the op counts
scale down. The five local 10M-op `lobsim difftest` runs recorded in PR #52
all finished with `mismatches: 0`. `.github/workflows/nightly.yml` runs five
10M-op seeds from the workflow run id on a schedule and on demand; it is
never a required check.

### Flow files and lobsim

`tools/flowfile/flow_record.hpp` defines the frozen wire format:
a 32-byte header (`LOBFLOW1`, version 1, 40-byte records, count, reserved) and
40-byte little-endian records (op, side, type, reserved, id, price, qty,
timestamp). `tools/flowfile/flowfile.hpp`/`.cpp` add the I/O on top: a
buffered `FlowWriter` that rewrites the header count on close, a streaming
`FlowReader` that distinguishes a truncated file from a count mismatch and
validates enum bytes, CSV read/write with words for enums and line-numbered
errors, and the `FlowErrorInfo` error model (no exceptions on the read path).
`tools/flowfile/replay_config.hpp` holds the shared default replay config
`{1, 200000, 1048576, 200000}`.

`lobsim gen` produces a seeded synthetic flow (Poisson arrivals, random-walk
mid, optional cancels/modifies and market/IOC shares, binary or CSV), and
`lobsim replay` drives either engine and prints counts plus an optional
digest and trade CSV. `scripts/check_determinism.sh` generates one flow and
compares every available engine's digest across debug and release;
`tests/test_lobsim_e2e.cpp` and `tests/test_lobsim_e2e_fast.cpp` cover
round trips, CSV/binary equivalence, the trade CSV row count, digest
formatting, and fast/reference agreement on generated and Tardis flows.

### Tardis conversion

`scripts/tardis_fetch.py` streams the public Binance ETHBTC
`incremental_book_L2` file for 2026-10-01 and keeps the first hour; the result
is committed as `tests/data/tardis_sample.csv.gz` (18,774 rows, ~0.17 MB).
`lobsim convert-tardis` (with `tools/flowfile/tardis.hpp`/`.cpp`) parses
decimals exactly with integer arithmetic (no `double`), keeps per-level
synthetic queues (increase → add at the back, decrease → reduce/cancel from
the back, 0 → cancel all, snapshot after deltas → clear and reload), skips
rows before the first snapshot and cancels crossed opposite levels before
adding, so replay produces no trades. The reference replay digest is
committed as the golden `tests/data/tardis_sample.digest`
(`0xea3b4198b591dd6d`). The sample is documented as a realistic price process,
not real order flow.

### Benchmark harness

`bench/` holds the measurement machinery and the suites:

- `timer.hpp`: `cntvct_el0`/`mach_absolute_time` counter, a cross-check
  between the declared `cntfrq_el0` rate and the mach timebase, and a
  measured back-to-back timer overhead.
- `latency.hpp`: an HDR histogram (1 ns … 10 ms, 3 significant figures). If
  the counter is at least 1 GHz, one sample per operation records elapsed
  minus measured overhead; otherwise batches of 16 operations are timed and
  the batch is divided by 16, and the result is labelled. The recorder
  reports p50/p90/p99/p99.9/max and a mean.
- `suites.cpp`: `AddOrder_NonCrossing`, `AddOrder_Crossing/1..3`,
  `Cancel_Resting`, `Market_Sweep/1..3` and `MixedFlow`. Each suite runs a
  setup-time sanity check with a counting listener (the timed orders really
  rest, cross the intended number of levels, cancel the intended order, or mix
  rests/trades/cancels) before timing anything.
- The benchmark result carries the percentiles and the mode as user counters
  (`p50_ns`, `p90_ns`, `p99_ns`, `p999_ns`, `max_ns`, `mean_ns`, `mode`) and
  throughput as `items_per_second` via `SetItemsProcessed`.
- `scripts/run_benchmarks.sh` is the only sanctioned way to produce
  publishable numbers: it builds release, runs `lob_bench --self-test`, runs
  five repetitions and renders `bench.md` with `scripts/bench_report.py`. It
  refuses to start unless the machine is idle (load average, compiler
  processes, runner workers) unless forced. `bench/testdata/sample_bench.json`
  is a renderer fixture taken on a busy machine, not publishable data.

## Key decisions and reasoning

- **The event contract is the specification.** Both engines were written from
  `docs/architecture.md` §4; the fast engine was never consulted while writing
  the reference engine, so agreement in the differential test is evidence, not
  tautology.
- **The reference engine is naive on purpose.** A list inside a map plus an
  unordered-map index is the most obviously correct representation, and it
  fixes the semantics; performance ideas are not allowed into it.
- **Capacity failures are cancels, not rejects.** §4.1 step 6 says an
  unrestable remainder emits `on_cancel(id, remainder)`, so
  `RejectReason::CapacityExceeded` stays unused in both engines.
- **`max_levels` is per side.** The level cap is checked against the side the
  order would rest on, matching the fast-engine ladder plan.
- **The generator avoids `std::*_distribution` and floating point.** Standard
  distributions are not portable across libraries, and the differential test
  must be reproducible by seed on any standard library and build type.
- **Digest comparison is not circular.** `DigestBuilder` only hashes bytes;
  each engine walks its own structures, and the byte layout is pinned by
  golden values computed outside C++ in Epic 0.
- **A digest-only fault is injected by perturbing the digest.** The frozen
  reference API cannot reorder two resting orders without emitting events, and
  a digest-only difference is exactly what must not emit events; the harness
  self-test documents this.
- **Small default config for `difftest`.** A digest comparison after every
  operation makes runtime grow with the live book, so the default config is
  small enough that capacity, level-cap and price-edge paths are all hit in a
  default run; production-scale runs use overrides or the nightly.
- **Long runs never gate merges.** The 10M-op nightlies live in their own
  workflow, fail quietly off the required checks, and derive their seeds from
  the run id so any failure is reproducible from the printed seeds.
- **Benchmark honesty.** The measurement harness reports its timer frequency,
  its overhead and its mode; sanity checks run before timing; and publishable
  numbers are only produced by the idle-machine script, so the repository
  never carries numbers that were taken on a busy machine.

## Interfaces for future epics

### Price ladder (Epic 2 array ladder)

`include/lob/price_ladder.hpp` is the seam: callers only use
`find(price)`, `find_or_insert(price)` (which returns `nullptr` when a new
level would exceed `max_levels`), `erase_if_empty(price)`, `best()`,
`best_price()`, `level_count()`, `max_levels()`, `empty()` and
`for_each_best_first(fn)`, with `Level { head, tail, total_qty, order_count }`
as the level type. Epic 2 replaces the `std::map` inside `PriceLadder` with an
array indexed by `price - min_price` plus a two-level occupancy bitmap for
best-level search; the public methods and `Level` stay the same, so the
matching code, the digest walk and the differential tests are untouched. The
array ladder must preserve the same semantics (per-side `max_levels`, best
first iteration, `kNullIndex`-terminated queues).

### `lobsim difftest` CLI

The CLI contract other tooling can rely on: flags exactly as listed above;
stdout ends with the `key: value` summary (`engine-a`, `engine-b`, the four
config keys, `ops`, `seed`, `mismatches`, `wall_ms`); `--progress` writes to
stderr; exit 0/1/2 for clean/mismatch/usage error. A mismatch report is
preceded by the shrinking block, whose ops are printed as pasteable
`lob::opgen::Op` initializers.

### Flow format

The wire format is frozen in `tools/flowfile/flow_record.hpp` and documented in
`docs/architecture.md` §6: 32-byte header and 40-byte records, little-endian,
field by field. The reader/writer API in `flowfile.hpp` adds buffered I/O, an
error enum plus message, streamed CSV, and a `CsvSink` so replay never
materializes a file. Any future consumer (Python bindings, converters) should
go through these headers rather than re-encoding the format.

### Benchmark JSON

`lob_bench` writes Google Benchmark JSON to `bench.json` next to the
executable (or `--benchmark_out`). Every benchmark entry carries `real_time`,
`items_per_second`, and the user counters `p50_ns`, `p90_ns`, `p99_ns`,
`p999_ns`, `max_ns`, `mean_ns` and `mode` (numeric: 0 = one sample per
operation, 1 = batches of 16). The context block carries the machine,
compiler, flags, repetitions, timer frequency, timer overhead and the macOS
caveat. `scripts/bench_report.py` renders that contract; a benchmark that ran
without the percentile counters is an error, not a silent blank row.

## Deviations from the plan

- The architecture document's digest section described a `begin()` method;
  the frozen builder writes the version byte in its constructor. §5 is
  corrected by this document's companion change (the Epic 0 write-up recorded
  the discrepancy).
- The reference engine includes `lob/order_book.hpp` solely for `LevelInfo`
  (decision C23); §8 is updated accordingly.
- `RejectReason::CapacityExceeded` exists but is unused; the contract routes
  unrestable remainders to `on_cancel`.
- `lobsim difftest`'s default config is its own small
  `{1, 500, 512, 16}` rather than `default_replay_config()`, because the
  shared replay default (1M orders, 200k levels) would never hit capacity in a
  useful time; the CLI accepts overrides for all four limits.
- The digest-only fault in the harness self-test is a digest perturbation
  rather than a physical queue swap, because the frozen reference API cannot
  reorder resting orders without emitting events.
- `scripts/run_benchmarks.sh` runs `AddOrder|Cancel|Market` plus `MixedFlow`
  so one JSON feeds both the latency and the throughput tables of the report.
- `tests/test_interfaces.cpp`'s stub-era "empty book after operations"
  expectation was trimmed to query a freshly constructed book when the
  reference engine landed; a stub cannot pass a real engine's API.
- The TSan preset exists and builds, but no CI job runs it; sanitizer
  coverage in CI is ASan+UBSan.

## Known limitations and deferred items

- **No published benchmark numbers yet.** The committed
  `bench/testdata/sample_bench.json` is a renderer fixture taken on a busy
  machine; publishable numbers come from `scripts/run_benchmarks.sh` on an
  idle machine later.
- **The tree price ladder is still the Epic 1 baseline.** The array ladder and
  the two-level occupancy bitmap (Epic 2) are not implemented; only the
  interface seam exists.
- **The nightly workflow has not been dispatched end to end from `main` yet.**
  It is validated with actionlint and its shell body was exercised locally;
  the first scheduled run (or a manual dispatch) is still to produce a green
  10M-op run on the default branch.
- **The Tardis sample is synthetic order flow.** It is an L2-delta
  reconstruction that is deliberately trade-free; it is a realistic price
  process, not real order flow, and it is used only for determinism and
  cross-engine checks.
- **Deferred beyond Epic 1:** FOK and post-only orders, stop orders,
  self-trade prevention, multiple instruments, persistence, networking,
  multithreading, Python bindings, Docker, and README.md.
- **Sanitizer-unsafe tests scale down or skip.** The differential tests lower
  their op counts under ASan/TSan, and the allocation test skips there
  entirely; macOS sanitizer interceptors make exact allocation counts
  unreliable.
