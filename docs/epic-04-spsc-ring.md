# Epic 04 — the lock-free SPSC ring

## Goal

Every Phase 3 service in this project has the same shape: one thread owns the
book and must never block, and another thread talks to the network. Handing
work between them through a mutex or an allocating queue would put a lock and a
possible `malloc` on the path that decides fill latency, which contradicts the
engine's own rule of no heap allocation and no hidden waits on the hot path.

Epic 04 provides the handoff primitive — a bounded lock-free single-producer
single-consumer ring buffer — plus the two harnesses that make a claim about it
possible: a stress binary that proves order and integrity at volume, with a
two-thread unit test that runs the same contract under ThreadSanitizer, and an
enqueue-to-callback latency harness that measures the ring plus the engine
event it carries. The ring is header-only, allocates nothing after
construction, and is the only queue the Phase 3 servers need between their
threads; a wake pipe and a handful of atomics signal wake-ups and shutdown
(decision C48).

## How it works

### The ring layout

`include/lob/spsc_ring.hpp` defines `lob::SpscRing<T, Capacity>`:

- **Fixed capacity, compile-time.** `Capacity` is the number of elements, must
  be a power of two and at least two (`static_assert`), so the slot index is
  `index & (Capacity - 1)` with no division or modulo. `T` must be trivially
  copyable (`static_assert`): elements cross threads as bytes, and a type with
  a nontrivial copy constructor could not be moved that way.
- **Inline storage.** The elements live in a `std::byte buffer_[Capacity *
  sizeof(T)]` member, aligned to `kSpscCacheLineSize` and to `T`. `try_push`
  `memcpy`s the caller's value into a slot and `try_pop` `memcpy`s it out, which
  is safe for trivially copyable types and lets the ring hold types that are
  neither default constructible nor assignable. Construction allocates nothing
  (the buffer is a member) and `try_push`/`try_pop` never allocate, never throw
  (`noexcept`) and never call into the operating system.
- **Free-running counters.** `tail_` is the number of elements ever pushed and
  `head_` the number ever popped, both `std::atomic<std::size_t>` (the class
  asserts the type is always lock-free). Full means `tail - head == Capacity`,
  empty means `tail == head`, and both differences are unsigned arithmetic
  modulo 2^64, which is exact as long as fewer than 2^63 elements are in flight.
- **API.** `try_push(const T&) -> bool`, `try_pop(T&) -> bool`,
  `size_approx()`, `capacity()`. Both operations report failure instead of
  waiting: a full ring leaves the value untouched and returns false, empty
  returns false and leaves the output untouched. Copy and move are deleted, so
  a ring is never accidentally duplicated. `size_approx()` is an exact count of
  the published indices that may lag a concurrent push or pop by one element.
  The ring has no `empty()`, no `full()` and no blocking wait, because a
  blocking or predicating API on the shared indices would reintroduce exactly
  the races the cached copies exist to avoid; callers poll.

### Cache-line separation

A ring's two hot words are written by different cores. If `head_` and `tail_`
share a cache line, every push and pop bounces that line between cores
(false sharing) and the ring becomes slower than a small mutex.

The header defines `lob::kSpscCacheLineSize = 128` and explains the choice:
Apple M-series performance cores have 128-byte L1 data cache lines and the
hardware prefetches the adjacent line, so two indices 64 bytes apart still share
a prefetch pair. 128 bytes is therefore the smallest padding that actually
separates the producer's and the consumer's hot words on this machine.
`std::hardware_destructive_interference_size` is 256 bytes with this toolchain,
a conservative upper bound rather than a measured line size; using it would only
add padding, and on other targets the constant is still a multiple of every
common cache line, so it is safe, just not minimal.

The members are laid out so each cache line belongs to exactly one thread:

| Line | Members | Owner |
| --- | --- | --- |
| 0 | `head_`, `cached_tail_` | consumer |
| 1 | `tail_`, `cached_head_` | producer |
| 2… | element buffer | both, during their own slot access |

### Cached indices and the acquire/release argument

Each side keeps a plain (non-atomic) copy of the other side's index:
`cached_head_` for the producer, `cached_tail_` for the consumer. The common
full and empty checks use the local copy and therefore do not touch the other
core's cache line at all. The cross-core index is re-read only when the local
view says full (producer) or empty (consumer), in which case it is loaded with
`std::memory_order_acquire`.

The header's comment block states the publication argument, and the
implementation is short enough to read against it:

1. The producer writes the element into the slot, then `release`-stores the new
   `tail_`. The release store publishes every preceding write, so a consumer
   that `acquire`-loads that `tail_` value and sees it also sees the element
   bytes. Nothing else orders the element against the index.
2. The consumer `acquire`-loads `tail_` (only when its cached copy says the
   ring is empty), reads the slot, then `release`-stores the new `head_`. The
   release store signals that the slot is free; the producer `acquire`-loads
   `head_` before reusing a slot, so it cannot overwrite an element the consumer
   has not finished reading.
3. `cached_head_` is read and written only by the producer and `cached_tail_`
   only by the consumer, so they are not atomic and cannot race.
4. No `seq_cst` is used or needed: the pairwise synchronisation is a
   release/acquire edge between exactly two threads that touch disjoint members.

`size_approx()` reads `head_` first and `tail_` second, both `acquire`: the
consumer index only moves towards (never past) the producer index, so the later
`tail` read is at least as large and the unsigned difference cannot wrap
between the two loads.

### The stress binary and TSan coverage

`tools/spsc/stress.cpp` builds `spsc_stress [--messages N]` (default
100,000,000 messages):

- A producer thread pushes `Message{sequence, payload}` into
  `SpscRing<Message, 4096>`; the consumer pops and validates strict sequence
  order plus a splitmix64-style bijection of the sequence as the payload, so a
  single corrupted, duplicated or reordered word fails the run.
- After the first violation the consumer keeps draining without validating, so
  a producer blocked on a full ring can always finish and the `join` cannot
  deadlock.
- While the ring is full or empty the tools spin with a `yield` on aarch64
  (`pause` on x86) instead of calling `sched_yield`.
- Output is the message count, ring capacity, elapsed time,
  `order ok, content ok` and the throughput in messages/s and ns/message.
  Exit code 1 reports the first sequence or payload violation with the expected
  and actual values; exit code 2 is a usage error.

The binary is a tool, not a ctest test: the default 100-million-message run is
too long for the CI budget. ThreadSanitizer coverage comes from the two-thread
unit test instead. `tests/test_spsc_ring.cpp` contains six tests:

| Test | What it pins |
| --- | --- |
| `CapacityIsReportedAndStartIsEmpty` | `capacity()`, an empty start, a failed pop leaving its output untouched |
| `PushAndPopAreFifo` | FIFO order for a short run |
| `FullBoundaryRejectsExactlyAtCapacity` | exactly `Capacity` pushes succeed, the next fails, and one pop frees one slot |
| `WrapsAroundManyTimesWithoutLosingTheOrder` | 2000 fill/drain rounds that wrap every slot many times, checking `size_approx()` after every round and the exact-full boundary whenever the ring fills |
| `TwoThreadsPreserveOrderAndContent` | 10,000,000 messages with sequence and payload validation (200,000 under ASan/TSan, where the count would otherwise dominate the run) |
| `HeadAndTailLiveOnDifferentCacheLines` | via a test-only friend probe: `head_` at offset 0, `tail_` at least one cache line away, each cached copy on its owner's line, buffer after the producer's line |

The `tsan` CI preset builds and runs this suite with ThreadSanitizer
instrumentation, and the `tsan` job is a required check. A data race in the
ring's ordering would therefore fail CI rather than wait for a stress run.

### The enqueue-to-callback latency harness

`tools/spsc/latency.cpp` builds `spsc_latency [--messages N] [--seed S]
[--latency-mode auto|per_op|batchN]` (default one million messages, seed 1).
It measures the thing the ring exists for: the time from a command being
enqueued by the feed thread to the engine listener's `on_accept` firing for
that command.

- The feed thread pulls operations from the seeded
  `lob::opgen::OpGenerator` (default mix, the flow tools' default
  configuration) and pushes them into `SpscRing<Event, 256>`. The ring is
  deliberately small: with a huge ring the producer runs far ahead and the
  histogram measures queue depth instead of handoff latency. Large enough that
  neither thread waits for most messages.
- The enqueue tick is read immediately before the push attempt that succeeds.
  Time spent retrying a full ring is producer backpressure, not handoff
  latency, so it is not counted.
- The engine thread pops events and applies them to a `lob::OrderBook` with an
  `AcceptLatencyListener`; `on_accept` reads the counter and records
  `callback - enqueue`. Only accepts are sampled; cancels and modifies are
  applied to the book and skipped by the histogram.
- Samples go into an HDR histogram covering 1 ns to 10 ms at three significant
  figures, the same range and precision `lob_bench` uses. The timer overhead
  measured at startup is subtracted from each sample, and samples above the
  10 ms ceiling are recorded at the ceiling and counted separately.

**Batch mode.** On this machine the counter (`cntvct_el0`) steps by about
41.7 ns even though it reports 1 GHz, so a per-operation timestamp is
meaningless: every operation rounds to the same handful of values (decision
C32). The harness therefore shares `lob_bench`'s policy, with the thresholds
and bounds in `bench/latency.hpp`:

- per-op sampling only when the measured granularity is at most 2 ns
  (`kPerOpMaxGranularityNs`);
- otherwise the smallest power-of-two batch of at least 16 operations
  (`kMinBatchSize`) whose measured cost spans at least 50 timer steps
  (`kBatchGranularityTarget`), capped at 1024 (`kMaxBatchSize`);
- the reported sample is the mean of up to N accepted events, so the label
  (`per_op` or `batchN`) is part of the number, and the partial final batch is
  flushed;
- auto mode calibrates the engine event by timing a submit followed by its
  cancel on a scratch book and halving the pair. Calibrating on the real
  submit/listener path rather than on the ring plumbing keeps the chosen batch
  comparable to `lob_bench`'s choice for an equivalent operation: the plumbing
  is about 2 ns and would always select the 1024 cap.

The tool prints the source and seed, messages and accepted count, events per
second, ring capacity, the timer granularity and calibrated event cost, the
mode, the sample count (and any ceiling overflows), p50/p90/p99/p99.9/max/mean
in nanoseconds, and a final line stating that a run on a shared machine is
exploratory and not a publishable number.

### Published numbers

The figures below come from one `scripts/run_phase23_benchmarks.sh` run that
waited for an idle machine and took five repetitions of
`spsc_latency --messages 1000000`; the median run by p50 is reported. Latency in
nanoseconds.

| Statistic | ns |
| --- | --- |
| p50 | 196 |
| p90 | 213 |
| p99 | 705 |
| p99.9 | 84,543 |
| max | 182,271 |

| Field | Value |
| --- | --- |
| Events per run | 1,000,000 |
| Samples (median run) | 2,462 |
| Throughput (median run) | 4,898,954 events/s |
| p50 across runs | 187–206 ns |

Apple M5 (Mac17,2, arm64, 32 GiB), macOS 27.0.1 (26A434), Apple clang 21.0.0
(clang-2100.3.34.2), `-O3 -DNDEBUG`, `batch256` sampling, 5 runs of 1,000,000
events, 2026-10-29 (UTC), commit `3acb776`; 1-minute load average 1.97 before
the run and 3.21 after it.

What the number does and does not include: it is the enqueue-to-`on_accept`
handoff of one event, so it contains the producer's push, whatever wait the
consumer made it do, the pop, the book update and the listener call — and no
network I/O, which is what makes it a property of the ring and the engine rather
than of a socket. Because every sample is the per-op mean over 256 accepted
events, a preemption inside a batch is diluted across it: the p99.9 and max are
batch means and the single-event tail is worse than they look. The p50 moving
between 187 ns and 206 ns across the five runs is the resolution of a machine
with no thread pinning, so treat anything under about 10% as equal.

## Key decisions and reasoning

- **A ring, not a mutex-protected queue.** The engine thread must never wait on
  a lock owned by a network thread. A bounded ring with `try_push`/`try_pop`
  keeps the wait in the caller's control (it can poll, batch or drop) and makes
  the cost of a handoff a handful of instructions.
- **Single producer, single consumer, by design.** That is exactly the shape of
  every Phase 3 server: the network thread feeds the engine, the engine feeds
  the network thread (decision C54). A multi-producer queue would need CAS
  loops and per-slot sequence numbers for a concurrency level the architecture
  does not have.
- **Power-of-two capacity.** A mask replaces a division in the two hottest
  operations, and the capacity is a compile-time constant so the compiler can
  fold the mask. The price is that capacity rounds up to the next power of two.
- **128-byte padding instead of the standard constant.** Measured on the target
  machine, not assumed from a portable upper bound; the header records both
  numbers so a reader on another platform can see why the constant is safe.
- **Cached indices.** The common full/empty decision uses only thread-private
  memory; the cross-core load happens only when the answer may have changed.
- **Raw byte storage with `memcpy`.** It allows non-default-constructible
  message types, keeps the element path a single copy per direction, and avoids
  any question of object lifetime in uninitialised slots.
- **Only relaxed, acquire and release.** Sequential consistency would add
  fences on the platform (and a full barrier to both operations) with no
  observable benefit for a two-thread handoff; the release/acquire pairs above
  are the complete argument.
- **A small ring in the latency harness, a big one in the stress harness.** The
  stress harness wants throughput, so it should rarely see a full ring; the
  latency harness wants to measure the handoff, so a queue that never fills
  would let the producer run ahead and report queue depth instead.
- **Calibrate the batch on the engine event.** It keeps the harness's mode
  comparable with the benchmark harness, which is what makes the two sets of
  numbers readable side by side.
- **Mirror, do not link, the mode selector.** `select_latency_mode` lives in
  `bench/latency.cpp`, which is not linked into the tools; the tool reimplements
  the few lines of selection and includes the thresholds and bounds from
  `bench/latency.hpp`, so the two harnesses cannot drift on the values that
  matter.

## Interfaces for future epics

- **The gateway handoff (Phase 3).** `lob_gateway` uses three rings, all in
  `net/gateway/gateway_state.hpp`:
  - `commands` (network → engine, `2^14` entries) carries one decoded client
    command per slot: a fixed-size `Command` struct, at most 64 bytes, holding
    kind, side, type, session, order id, price and quantity. It is the ring
    depth that bounds how far a client can run ahead of the book.
  - `responses` (engine → network, `2^16` entries) carries an encoded frame plus
    its session id. It is larger than the command ring because one command can
    emit several responses: a sweeping order produces one Fill per maker plus
    the taker's remainder.
  - `md` (engine → network, `2^16` entries) carries encoded market-data frames
    with their gap-free sequence number already stamped by the engine; the
    network thread fans them out to subscribers.

  The ring API drives the backpressure policy on each side. The engine never
  drops a response or a market-data event: when a downstream ring is full it
  yields and retries (`push_response`, `publish_md`). The network thread bounds
  a client that outruns the engine by buffering its decoded input up to a
  fixed limit and disconnecting it beyond that, rather than by blocking the
  engine on the command ring. Wake-ups go through a self-pipe that is written
  only when it is not already pending, so a burst of events does not turn into
  a syscall per event.
- **Any future single-producer/single-consumer seam.** A second engine worker, a
  market-data fan-out or an offline backtester feeding a book reuses the same
  shape; the requirements are a trivially copyable message type and a
  compile-time capacity.
- **The latency policy.** `bench/latency.hpp` is the shared home of the mode
  thresholds and bounds. A future harness that reports per-operation latency
  includes that header and follows the same label discipline instead of
  inventing its own sampling rule.
- **Test hooks.** The friend `SpscRingLayoutProbe` keeps the layout assertions
  possible from a test without exposing indices in the public interface; a
  future variant of the ring can keep the same probe contract.

## Deviations

- `kSpscCacheLineSize` is a project constant (128 bytes), not a query of the
  platform's cache line size. A portable upper bound is available
  (`std::hardware_destructive_interference_size`, 256 here) but is not the
  measured line size.
- The ring has no `empty()`/`full()` predicates and no blocking `push`/`pop`;
  callers poll and handle backpressure. `size_approx()` is documented as
  approximate, not as a synchronisation primitive.
- The stress binary is not registered with ctest. The 100-million-message
  default is a manual tool run; CI's race coverage is the two-thread unit test
  under the `tsan` preset, which runs 200,000 messages.
- The latency harness does not link `bench/latency.cpp`; it mirrors the mode
  selection and shares only the thresholds. The alternative — linking the bench
  library into the tool — would pull Google Benchmark and the reporting code
  into a binary that needs neither.
- The enqueue timestamp is read before the successful push attempt rather than
  once per message, which slightly understates the producer's own cost but
  keeps backpressure out of the sample. The reported number is a handoff
  latency, not a full round trip.
- The gateway inverts the producer/consumer wait: the network thread never
  blocks the engine on a full command ring (it buffers or disconnects the
  client), while the engine does spin-yield when a response or market-data
  ring is full, because dropping those events would break the client protocol
  and the market-data sequence. The ring itself stays non-blocking; the policy
  lives in its callers.
- The ring's element path is two `memcpy` calls. For the small command and
  response structs of the Phase 3 servers that is one or two machine words per
  direction; for a large `T` the copy cost is the caller's design choice.

## Known limitations

- **Latency numbers belong to one machine and one sampling mode.** The published
  figures come from a single idle Apple M5 run in `batch256` mode, so the tail
  columns are batch means and the numbers only mean something next to the
  machine, flags and commit quoted with them. There is no cross-machine
  comparison and no number taken from a container.
- **Machine-specific padding.** The 128-byte choice is justified for Apple
  M-series cores. On a target with 64-byte lines it over-pads harmlessly; on a
  target with larger lines it could under-separate, which is why the header
  records the reasoning rather than presenting the number as portable.
- **Fixed capacity.** Capacity is a compile-time constant and the ring never
  grows. A producer that outruns its consumer must drop, batch or apply
  backpressure; there is no overflow buffer.
- **The contract is not enforced.** The ring has no way to detect a second
  producer or a second consumer; violating the single-thread rule is a data
  race that the type system cannot catch. Only the TSan-instrumented test
  guards the two-thread case.
- **Element copies.** Trivially copyable payloads are copied into and out of the
  buffer. Large messages cost two copies and should be passed by index or
  pointer instead.
- **No fairness or priority.** A busy producer can starve the consumer of cache
  bandwidth during a burst; the QoS hint is the only scheduling control on
  macOS, and there is no pinning.
- **The stress harness is not a benchmark.** It measures moving 16-byte
  messages between two threads on one machine; it says nothing about network
  throughput or about a ring shared across sockets.
