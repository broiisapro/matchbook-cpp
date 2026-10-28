# matchbook-cpp — architecture

matchbook-cpp is a single-instrument limit order book and matching engine in C++20. Prices are integer ticks, quantities integer lots, matching is price-time priority, and the hot path (add, cancel, modify, match) does no heap allocation after construction. Correctness is established by a differential test against a deliberately naive reference engine; performance is reported as latency percentiles with the machine, compiler, flags and timer resolution that produced them.

## 1. Repository layout

```
CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json (optional)
.clang-format  .clang-tidy  .python-version  .gitignore
include/lob/
  types.hpp           Price, Qty, OrderId, Side, OrderType, RejectReason   (frozen after Epic 0)
  config.hpp          Config {min_price, max_price, max_orders, max_levels}  (frozen after Epic 0)
  listener.hpp        abstract Listener + NullListener                       (frozen after Epic 0)
  digest.hpp          DigestBuilder (byte spec §5)                            (frozen after Epic 0)
  order_book.hpp      OrderBook public API                                    (API frozen after Epic 0)
  pool.hpp            fixed-capacity object pool + PoolAllocator              (WS A)
  intrusive_list.hpp  intrusive doubly linked FIFO                            (WS A)
  id_map.hpp          open-addressing OrderId -> pool index                   (WS A)
  price_ladder.hpp    price-level container                                   (WS A)
src/order_book.cpp    fast engine                                             (WS A)
reference/reference_book.hpp/.cpp   naive reference engine, test-only target  (WS B)
tools/flowfile/       flow record spec (Epic 0) + binary/CSV reader/writer    (WS C)
tools/opgen/          seeded random operation generator                       (WS B)
tools/lobsim/         CLI: gen, replay, difftest, convert-tardis              (WS B: difftest.cpp; WS C: the rest)
bench/                Google Benchmark suites + HDR harness                   (WS D)
tests/                GoogleTest unit, differential, allocation tests; tests/data/ fixtures
scripts/              bootstrap_vcpkg.sh; bench_report.py, tardis_*.py (uv)
docs/                 architecture.md, epic-NN-<slug>.md
```

CMake targets (all defined in Epic 0 with placeholder sources): `lob` (static library), `lob_reference` (static, test-only), `lob_flowfile`, `lob_opgen`, `lobsim` (exe), `lob_bench` (exe), `lob_tests` (gtest exe), `test_alloc` (separate gtest exe, because it replaces global operator new). Every target compiles with `-Wall -Wextra -Wpedantic -Werror`. Test, bench, reference, opgen and flowfile targets collect sources with `file(GLOB ... CONFIGURE_DEPENDS)` so adding a file never requires editing `CMakeLists.txt`.

## 2. Build and toolchain

- Apple clang from Xcode (currently Xcode 27.1 / Apple clang 21), C++20, CMake ≥ 3.28 (Homebrew currently ships 4.4.x), Ninja.
- `CMakePresets.json` `"version": 8`. Configure presets: `release` (`-O3 -DNDEBUG`, `-march`/`-mcpu=native` NOT used so numbers are reproducible on any Apple Silicon; flags printed by the bench), `debug`, `asan` (`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`), `tsan` (`-O1 -g -fsanitize=thread`). Matching build and test presets. Binary dirs `build/<preset>`.
- vcpkg manifest mode, `builtin-baseline` `9e593bb18ea69cc5095e012465dcd675a822ed0d` (release 2026.07.29): `gtest` 1.17.0, `benchmark` 1.9.5, `hdr-histogram` 0.11.10, `xxhash` 0.8.3. `scripts/bootstrap_vcpkg.sh` does a full clone of microsoft/vcpkg into `./vcpkg`, checks out the pinned commit and runs `bootstrap-vcpkg.sh -disableMetrics`; idempotent. Presets set `CMAKE_TOOLCHAIN_FILE` to `${sourceDir}/vcpkg/scripts/buildsystems/vcpkg.cmake`. If the `VCPKG_ROOT` environment variable is set, the bootstrap script clones/verifies the pinned vcpkg there and symlinks `./vcpkg` to it (CI and worktrees share one clone); it is safe to run concurrently (mkdir-based lock).
- Formatting: `uvx clang-format==23.1.3` (exact pin, same locally and in CI). clang-tidy from Homebrew `llvm`, advisory.
- Fallback if vcpkg bootstrap fails on runners (risk 3): pinned `Brewfile`; recorded as a decision if used.

## 3. Domain types and configuration

- `using Price = std::int64_t; using Qty = std::int64_t; using OrderId = std::uint64_t;`
- `enum class Side : std::uint8_t { Buy = 0, Sell = 1 };`
- `enum class OrderType : std::uint8_t { Limit = 0, Market = 1, IOC = 2 };` (FOK/PostOnly appended in Phase 2; values never renumbered.)
- `enum class RejectReason : std::uint8_t { DuplicateId = 1, UnknownOrder, InvalidQuantity, InvalidPrice, CapacityExceeded, ModifyNoChange };`
- `struct Config { Price min_price; Price max_price; std::uint32_t max_orders; std::uint32_t max_levels; };` Both engines enforce the same limits (§4.1), so they stay comparable even when limits bite.

## 4. Event contract (authoritative for both engines)

Listener (abstract class, virtual methods; `NullListener` is final and does nothing):

```
on_accept(OrderId id)
on_reject(OrderId id, RejectReason reason)
on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty)
on_cancel(OrderId id, Qty cancelled_qty)            // remaining qty removed from the book or not filled
on_modify(OrderId id, Price price, Qty new_qty)     // in-place reduce only
on_book_update(Side side, Price price, Qty new_level_qty)   // 0 = level gone
```

Callbacks are synchronous and must not call back into the book. Debug builds assert on reentrancy.

### 4.1 `submit(OrderId id, Side side, OrderType type, Price price, Qty qty)`
Validation, in this order, first failure wins, nothing else is emitted:
0. `type` is `FOK` or `PostOnly` → `on_reject(id, UnsupportedOrderType)` while the Phase 2 semantics below are unimplemented (interim behaviour, decision C47; see §4.1.1–§4.1.2 for the final contract).
1. `qty <= 0` → `on_reject(id, InvalidQuantity)`.
2. Limit, IOC, FOK or PostOnly with `price < min_price || price > max_price` → `on_reject(id, InvalidPrice)`. Market orders ignore `price` (callers pass 0).
3. `id` is a live resting order → `on_reject(id, DuplicateId)`. (Ids of filled or cancelled orders may be reused.)
Capacity (`max_orders`, `max_levels`) is not a validation step; it only matters if a limit remainder must rest (step 6).

Then:
4. `on_accept(id)`.
5. Match loop while remaining > 0 and the best opposite level crosses (buy: `ask <= price`; sell: `bid >= price`; market: any opposite level): take the head (oldest) maker at the best opposite level; `fill = min(remaining, maker.qty)`; emit `on_trade(maker.id, id, maker.price, fill)`; then `on_book_update(opposite side, maker.price, new level qty)`. A fully filled maker leaves the book silently (no `on_cancel`). A partially filled maker keeps its queue position.
6. Remainder > 0 (an order rests only if live orders < `max_orders` and, for a new price level, live levels < `max_levels`):
   - Limit: if capacity allows, rest at the back of its price level and emit `on_book_update(side, price, new level qty)`. If capacity does not allow, emit `on_cancel(id, remainder)` (the order was accepted and may have traded; the unrestable remainder is cancelled). Tested identically in both engines.
   - Market or IOC: `on_cancel(id, remainder)`.

### 4.1.1 `FOK` — fill or kill (final semantics, implemented in Epic 06)

**Interim (current):** rejected at validation step 0 with `UnsupportedOrderType`, in both engines, exactly as written in §4.1. Nothing else is emitted and the book is untouched.

**Final:** a limit order that must fill completely and immediately against the resting book or not trade at all.
1. Validation is the §4.1 sequence without step 0 (FOK is supported), with the FOK price bounds of step 2. Duplicate id, quantity and price rejects are unchanged.
2. `on_accept(id)` is emitted before any trade.
3. The kill check runs before the first trade is emitted: the available crossing quantity is the total quantity resting at opposite levels that the order crosses at its limit price (buy: every ask level `<= price`; sell: every bid level `>= price`). If that total is less than the submitted quantity, the order is killed: `on_accept(id)` is followed by `on_cancel(id, qty)` — the full submitted quantity, which never rested — and no `on_trade` or `on_book_update` is emitted. The book is unchanged.
4. If the crossing quantity is enough, the fill is performed exactly as in §4.1 step 5, in price-time priority, until the full quantity is filled. The event sequence is then identical to an IOC of the same side, price and quantity that fully fills: `on_accept`, one `on_trade` plus `on_book_update` per maker touched, no `on_cancel`.
5. A FOK order never rests and never hits the `max_orders` / `max_levels` capacity path: a fully filled FOK leaves nothing behind, and a killed FOK is cancelled as above.

### 4.1.2 `PostOnly` — rest or reject (final semantics, implemented in Epic 06)

**Interim (current):** rejected at validation step 0 with `UnsupportedOrderType`, in both engines, exactly as written in §4.1. `RejectReason::WouldCross` exists in the enum already so the final path needs no enum change.

**Final:** a limit order that is only allowed to add liquidity.
1. Validation is the §4.1 sequence without step 0 (PostOnly is supported), with the PostOnly price bounds of step 2. Duplicate id, quantity and price rejects are unchanged.
2. If the order would cross on arrival — i.e. the opposite side's best price crosses the limit price at submit time (buy: `best_ask <= price`; sell: `best_bid >= price`) — it is rejected with `on_reject(id, WouldCross)`. Nothing else is emitted: no `on_accept`, no trade, no book update, and the id is not resting.
3. Otherwise it rests exactly like a limit order: `on_accept(id)` and then §4.1 step 6 for the full quantity, including the capacity rule (if `max_orders` or `max_levels` forbids resting, `on_cancel(id, qty)`).
4. The crossing check is against the book as it is when the operation is applied, not against a later state; an order that rests can of course be traded against by a later incoming order.

### 4.2 `cancel(OrderId id)`
Unknown id → `on_reject(id, UnknownOrder)`. Otherwise remove it: `on_cancel(id, remaining qty)`, then `on_book_update(side, price, new level qty)`.

### 4.3 `modify(OrderId id, Price new_price, Qty new_qty)` — `new_qty` is the new *remaining* quantity
1. Unknown id → `on_reject(id, UnknownOrder)`.
2. `new_qty <= 0` → `on_reject(id, InvalidQuantity)`.
3. `new_price` outside `[min_price, max_price]` → `on_reject(id, InvalidPrice)`.
4. Same price and `new_qty == remaining` → `on_reject(id, ModifyNoChange)`.
5. Same price and `new_qty < remaining` → reduce in place, keep queue position: `on_modify(id, price, new_qty)`, `on_book_update(side, price, new level qty)`.
6. Otherwise (price change or increase) → cancel + new: `on_cancel(id, remaining)`, `on_book_update(side, old price, new level qty)`, then exactly the §4.1 sequence (steps 4–6) for `submit(id, side, Limit, new_price, new_qty)` (accept, matching, resting at the back of the queue).

### 4.4 Queries
`best_bid()`, `best_ask()` (`std::optional<Price>`), `level_qty(Side, Price)`, `top_levels(Side, std::span<LevelInfo> out) -> size_t`, `order_count()`, `digest() -> std::uint64_t`.

## 5. Digest (byte spec)

Canonical byte stream, all integers little-endian, written field by field (never `memcpy` of structs):
1. Version byte `0x01`.
2. Bid levels from best (highest) to worst, then ask levels from best (lowest) to worst. For each level: `side u8` (0 bid, 1 ask), `price i64`, `order_count u32`, then for each order in FIFO order: `id u64`, `remaining_qty i64`.
Hash: `XXH64(bytes, len, seed = 0)`. Implemented once as `lob::DigestBuilder`, whose constructor resets the XXH64 state (seed 0) and writes the version byte; there is no `begin()` call. From there `level(side, price, order_count)`, `order(id, remaining_qty)` and `finish() -> u64` stream the rest of the bytes through `XXH64_state_t`, so the builder only hashes bytes. Each engine walks its own structures. Golden test: the empty book and a fixed 3-level book have committed expected values.

## 6. Flow file (byte spec)

Header, 32 bytes: `magic[8] = "LOBFLOW1"`, `version u32 = 1`, `record_size u32 = 40`, `record_count u64`, `reserved u64 = 0`.
Record, 40 bytes, little-endian:

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | op | u8 | 1 submit, 2 cancel, 3 modify |
| 1 | side | u8 | 0 buy, 1 sell (ignored for cancel/modify) |
| 2 | type | u8 | 0 limit, 1 market, 2 IOC (submit only) |
| 3 | reserved | u8[5] | zero |
| 8 | id | u64 | |
| 16 | price | i64 | ticks; 0 for market/cancel |
| 24 | qty | i64 | lots; 0 for cancel |
| 32 | ts_ns | u64 | source timestamp, informational, never affects matching |

CSV form, header `op,side,type,id,price,qty,ts_ns` with op/side/type as words (`submit,buy,limit,...`). Reader validates magic, version and record size and fails loudly on a truncated file. The record struct and constants live in `tools/flowfile/flow_record.hpp` (Epic 0).

## 7. Fast engine (WS A)

- **Order pool:** fixed capacity `max_orders`, contiguous array, free list of 32-bit indices, constructed up front.
- **Intrusive FIFO:** `prev`/`next` 32-bit indices inside the order node; level holds head, tail, total qty, count. Removal is O(1); during the match loop the next pointer is read before a fully filled maker is unlinked and freed, and the node is freed only after its events are emitted.
- **Id map:** open addressing, capacity = next power of two ≥ 2 × `max_orders`, linear probing, backward-shift deletion (no tombstones), never rehashes. Considered `std::unordered_map` (allocates one node per insert even after `reserve`) and dense engine-assigned ids (doesn't fit caller-supplied 64-bit ids). 
- **Price levels, Epic 1 baseline:** `std::map<Price, Level, Cmp, PoolAllocator<...>>`, one map per side, with a fixed-capacity node-pool allocator sized from `max_levels`; best level is `begin()`. Considered a sorted `std::vector` of levels with best at the back (fast and simple, but leaves less to optimize and is not the tree baseline the PRD describes) and starting directly with the array ladder (no before/after story).
- **Price levels, Epic 2 optimization:** array indexed by `price - min_price` over the whole configured range, plus a two-level occupancy bitmap (`std::countr_zero` for asks, `std::countl_zero` for bids). Memory is bounded by Config (e.g. 2 sides × 65,536 ticks × 32 B ≈ 4 MB). Same semantics, so the differential test and digests are unchanged.

## 8. Reference engine (WS B)

`std::map<Price, std::list<Order>>` per side plus a `std::unordered_map<OrderId, std::list<Order>::iterator>` — whatever is most obviously correct (allocation is fine here). It is never optimized and includes no fast-engine headers other than `types.hpp`, `config.hpp`, `listener.hpp`, `digest.hpp`, and may include `lob/order_book.hpp` solely for the `LevelInfo` struct (decision C23). It enforces the same Config limits by counting.

## 9. Differential testing (WS B)

`lob_opgen` produces seeded operation streams (std::mt19937_64 with explicit distributions implemented by hand, because standard distributions are not portable across library implementations). The mix is weighted toward crossing orders, modifies at the best level, cancels of queue heads and tails, IOC/market against an empty side, duplicate and unknown ids, and prices at the Config edges. Both engines receive each operation with a recording listener; after every operation the event vectors must be identical and digests equal. On mismatch: print seed, op index, the op, and the first differing event. `lobsim difftest --ops N --seed S` prints `mismatches: 0`. PR CI runs ≥ 1M ops (one seed); 10M × 5 seeds runs on demand / nightly.

## 10. Tools (WS C)

- `lobsim gen --orders N --seed S --out F [--cancel-ratio r] [--csv]`: Poisson arrivals, random-walk mid, configurable cancel ratio.
- `lobsim replay F --engine fast|reference [--digest] [--trades out.csv]`: prints ops, trades, final digest (hex), wall time.
- `lobsim convert-tardis in.csv.gz --out F [--tick 0.00001] [--max-rows N]`: Tardis `incremental_book_L2` (columns `exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount`; `amount` is the new absolute level size; skip until first snapshot; a snapshot after deltas clears the book). Exact decimal parsing to ticks. Per-level synthetic queues: increase → add delta at the back; decrease → reduce/cancel from the back; 0 → cancel all at the level; before adding at a price that would cross the opposite side, cancel the crossed opposite levels. Replay therefore produces no trades. Fixture: Binance ETHBTC 2026-10-01, first hour, committed with its golden digest.

## 11. Benchmarks (WS D)

- `lob_bench` (Google Benchmark): AddOrder (non-crossing), AddOrderCrossing (1–3 levels), Cancel, MarketOrder, MixedFlow (throughput). Book prepopulated before timing; pools sized so warm-up touches all memory.
- Latency percentiles: a separate harness records into HdrHistogram (1 ns – 10 ms, 3 sig figs). At startup it reads and prints the counter frequency. If ≥ 1 GHz: per-op timestamps (`isb` + `cntvct_el0` or `mach_absolute_time`) minus measured timer overhead. Otherwise: batches of 16 ops ÷ 16, labelled. Reports p50/p90/p99/p99.9/max, ≥ 5 repetitions, median run; JSON to `build/release/bench.json`.
- `scripts/bench_report.py` (uv) renders the Markdown table with CPU model, OS, compiler version, flags, run count, timer frequency, and the macOS caveat (no core isolation or pinning; QoS hint only).
- Published numbers are taken on an otherwise idle machine (no concurrent builds or CI jobs).

## 12. CI

GitHub Actions on self-hosted runners (`runs-on: [self-hosted, macOS, ARM64]`), concurrency group per branch with cancel-in-progress. Jobs: `build`, `test`, `asan`, `format`, `hygiene`, `tidy` (advisory). `hygiene` (separate workflow, also runs when a PR is edited) fails if the PR title, body or any commit message in the PR matches the patterns stored in repository variables, or if a commit's author/committer email differs from the expected identity variable. The patterns live only in repository variables, never in the repository. vcpkg uses the runner-local binary cache. After public ship: GitHub-hosted macOS ARM64 runners (`macos-26` or `xcode-27`).

## 13. Testing strategy

Unit tests per component; the classic-bug tests named in the PRD; the differential test; the allocation test (`test_alloc` binary, global operator new/delete replaced, counting armed only around the hot loop, skipped under sanitizers); ASan+UBSan run of the full suite in CI; determinism check (debug vs release digests on the same flow).

## 14. Wire protocol (Phase 3)

The Phase-3 services (gateway, market-data publisher, recovery service) share one binary protocol, built in `net/protocol/` as the `lob_net` static library: `messages.hpp` holds the message table and compile-time sizes, `codec.{hpp,cpp}` converts between bytes and messages, and `framer.{hpp,cpp}` turns an arbitrary byte stream into frames. Every integer is little-endian and every message is fixed size. Fields are written and read one at a time; no struct is ever `memcpy`'d, so the layout does not depend on host byte order, alignment or padding.

### 14.1 Common header

Every frame starts with this 16-byte header:

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | type | u16 | message id from §14.2 |
| 2 | length | u16 | body size in bytes, header excluded (a frame is 16 + length bytes) |
| 4 | seq | u64 | sender's per-connection sequence number, increasing by one per frame |
| 12 | reserved | u32 | zero; a non-zero value is a protocol error |

### 14.2 Messages

| Id | Message | Direction | Body | Body layout (offset: field) |
|---|---|---|---|---|
| 1 | NewOrder | client → gateway | 32 | 0: order_id u64, 8: price i64, 16: qty i64, 24: side u8, 25: type u8, 26: reserved u8[6] |
| 2 | Cancel | client → gateway | 16 | 0: order_id u64, 8: reserved u8[8] |
| 3 | Modify | client → gateway | 24 | 0: order_id u64, 8: price i64, 16: qty i64 |
| 4 | Heartbeat | client → gateway | 8 | 0: nonce u64 |
| 5 | Ack | gateway → client | 16 | 0: order_id u64, 8: reserved u8[8] |
| 6 | Reject | gateway → client | 16 | 0: order_id u64, 8: reason u8, 9: reserved u8[7] |
| 7 | Fill | gateway → client | 40 | 0: maker_id u64, 8: taker_id u64, 16: price i64, 24: qty i64, 32: reserved u8[8] |
| 8 | Cancelled | gateway → client | 16 | 0: order_id u64, 8: qty i64 |
| 9 | BookUpdate | publisher → subscriber | 24 | 0: price i64, 8: qty i64, 16: side u8, 17: reserved u8[7] |
| 10 | Trade | publisher → subscriber | 40 | 0: maker_id u64, 8: taker_id u64, 16: price i64, 24: qty i64, 32: reserved u8[8] |
| 11 | SequenceReset | publisher → subscriber | 16 | 0: next_seq u64, 8: reserved u8[8] |
| 12 | RecoveryRequest | subscriber → recovery | 16 | 0: from_seq u64, 8: to_seq u64 |

Reserved bytes must be zero; encoders write them as zero and decoders reject a non-zero value. Type ids are appended only and never renumbered. The largest body is 40 bytes, so a frame never exceeds 56 bytes.

`NewOrder.qty` is in lots and `price` in ticks; a market order sends price 0, which it ignores. `Modify.qty` is the new remaining quantity (not a delta). `BookUpdate.qty` is the new total at the level, 0 meaning the level is gone. `Cancelled.qty` is the quantity removed. `Fill.price` is the maker's resting price. `Heartbeat.nonce` is chosen by the client and echoed by the gateway's heartbeat, so the client can measure round-trip time. `SequenceReset.next_seq` is the first sequence number the subscriber should expect next; `RecoveryRequest` asks for the frames in `[from_seq, to_seq]` inclusive.

Enum-valued fields use the wire values of `Side`, `OrderType` and `RejectReason` from `lob/types.hpp`: side 0 buy / 1 sell; order type 0 limit, 1 market, 2 IOC, 3 FOK, 4 post-only (the last two are reserved by decision C47 and gain semantics in Epic 06); reject reason 1–7, covering `RejectReason` plus `UnsupportedOrderType`. Decoders reject a side, order type or reason outside those ranges.

### 14.3 Validation and framing

Decoding a header checks, in order: enough bytes (Truncated), reserved zero (BadReserved), `length <= 40` (Oversize), a known type id (UnknownType), `length == body size of that type` (BadLength). A typed decoder then checks the body's reserved bytes (BadReserved) and enum ranges (InvalidField); the header type must also match the requested message (WrongType). Only after every check passes are the caller's output value and sequence number updated.

`StreamFramer` accepts a stream in arbitrary chunks. It holds at most one frame (56 bytes, on the stack as a member) and never allocates: it copies header bytes until 16 are present, validates the header immediately — so a bad header is reported without waiting for a body — then copies the body and hands the complete frame to a `FrameSink` callback. `push()` reports how many input bytes it consumed; a sink that returns false stops the call (FramerResult::Stopped) and the caller resubmits the unconsumed bytes. A protocol error clears the buffer and latches until `reset()`; later pushes return Error without consuming input. The sink's span points into the framer's buffer and is valid only for the duration of the callback.
