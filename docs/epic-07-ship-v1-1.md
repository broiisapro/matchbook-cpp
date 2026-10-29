# Epic 07 — shipping v1.1.0

## Goal

v1.0.0 was the Phase 1 release: the engine, the reference engine, the
differential test, the tools and the published benchmark story. Phase 2 adds
the concurrency primitive Phase 3 needs, a Python binding and the two order
types real clients ask for. v1.1.0 is the point at which those three are
finished, documented, measured on an idle machine and tagged together.

Epic 07 is the release itself rather than a feature: it states what Phase 2
delivered, records the Phase 2 definition-of-done commands with the results they
produced, and gives the checklist that turns a green `main` into a tag. The
release is cut only after the wave-08 audit, so the numbers in the README and
the tags describe the same commit.

## How it works

### What Phase 2 delivered

**Epic 04 — the lock-free SPSC ring.** `include/lob/spsc_ring.hpp` provides
`lob::SpscRing<T, Capacity>`, the handoff between the gateway's network thread
and its engine thread: power-of-two capacity, inline storage, no allocation,
no exceptions and no kernel calls after construction. The indices are separated
by 128 bytes, the producer and consumer each cache the other's index, and
ordering uses acquire/release only. Two tools exercise it: `spsc_stress` moves
100 million messages through a 4,096-entry ring and checks strict sequence order
and a payload bijection on every one; `spsc_latency` measures the
enqueue-to-`on_accept` handoff. A two-thread unit test runs under the `tsan`
preset, and `tsan` is a required CI check that runs the whole suite.

**Epic 05 — Python bindings.** `python/pylob/` binds the C++ engine through
pybind11 and scikit-build-core; no matching logic is reimplemented. The
repository root is a uv workspace whose only member is `pylob`, so
`uv sync --frozen` builds the extension and `uv run` imports it. The binding
exposes `Config`, `OrderBook`, all three enums, a recording listener, the flow
reader and a two-engine `replay`, and `examples/replay_spread.py` replays a flow
record by record and plots the spread. The required `test` CI job builds the
bindings and runs their pytest suite on every pull request.

**Epic 06 — FOK and post-only.** The Phase 2 contract was frozen first (#90),
the default operation mix stopped emitting the new types (#111), the fast engine
(#113) and the reference engine (#114) implemented §4.1.1 and §4.1.2
independently, flow files and `lobsim gen` learned the types (#115), the
bindings exposed the enums (#117), and the all-types differential plus nightly
runs landed last (#116). A FOK runs its non-mutating availability walk after
`on_accept` and before the first trade; a post-only order that would cross is
rejected with `WouldCross` before it is accepted. Both engines reach the same
events and the same digest over the all-types mix.

**Cross-cutting.** The all-types differential is a release gate rather than a
one-off: `lobsim difftest --types all` runs 1,000,000 operations in pull-request
CI (100,000 under sanitizers), the nightly runs four combinations per seed and
ladder on both the array and tree presets, and the allocation test includes FOK
and post-only operations. The `linux` and `docker` CI jobs became required after
a green streak, so the Phase 2 release is also the first one whose Linux port
and container image are checked on every change.

### The Phase 2 definition of done

The wave-07 audit on 2026-10-28 ran the Phase 2 commands against both ladder
presets on `main` at `e060796`:

| Command | Result |
|---|---|
| `./build/release/lobsim difftest --ops 10000000 --seed 11 --types all` | `mismatches: 0` (array ladder) |
| the same with a fresh seed on `release-tree` | `mismatches: 0` (tree ladder) |
| `./build/release/spsc_stress --messages 100000000` | `order ok, content ok` |
| `uv sync --frozen && uv run python -c "import pylob"` | imports; prints the binding version |
| `uv run --frozen pytest -q python/pylob/tests` | passes |
| `uv run python examples/replay_spread.py --flow … --out …` | writes a non-empty PNG |
| `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan` | full suite green under ThreadSanitizer |

The audit's own TSan invocation used a mistyped `ctest -R -i ring`, which ctest
rejects; that was a fault in the audit script, not in the product. The evidence
for TSan coverage is the required `tsan` job, which had already run the whole
suite — ring tests included — green on every wave-07 pull request and on
`main`, plus the local preset run in this table.

### Release checklist for v1.1.0

The tag is cut by the orchestrator's release script after the wave-08 audit
(decision C60), on the same commit as the epics' own tags. In order:

1. **Phase 2 merged.** Epics 04, 05 and 06 are merged, the wave-08 checkpoint
   fixes (the `WouldCross` wire range, idle heartbeats, the wiring test) are in,
   and `main` is green on every required check: `build`, `test`, `asan`,
   `format`, `hygiene`, `tsan`, `linux` and `docker`.
2. **Docs merged.** `docs/epic-04-spsc-ring.md`, `docs/epic-05-python-bindings.md`
   and `docs/epic-06-fok-post-only.md` are on `main`, and the README's Phase 2
   sections describe what is actually shipped.
3. **Idle-machine numbers.** With no agents, builds or CI running, run
   `scripts/run_phase23_benchmarks.sh` on `main` and record the commit. Ring
   enqueue-to-callback and native closed-loop wire-to-wire numbers come only
   from that run, with the machine, compiler, flags and mode copied from it
   (decisions C42 and C59). The script refuses to start on a busy machine.
4. **Placeholders replaced.** The README's marked ring-latency and
   wire-to-wire blocks are filled from that run only. If a number moved from an
   earlier run, the release notes say so rather than averaging.
5. **Fresh-clone verification.** Clone `main` into a temporary directory,
   bootstrap vcpkg, build and test `release`, `release-tree`, `debug`, `asan`
   and `tsan`, run one 10,000,000-operation all-types difftest per ladder, run
   the Phase 2 definition-of-done commands above, and run both the native
   end-to-end wiring test and the literal Compose demo. This is the same audit
   performed at each epic close.
6. **Repository hygiene.** Confirm the license and README links, the committed
   docs, and that no secrets, build outputs or local orchestration files are
   tracked; confirm the attribution scan and the commit-identity check are
   green.
7. **Tags.** Create the annotated tags `epic-04-complete`,
   `epic-05-complete`, `epic-06-complete` and `v1.1.0` on the release commit and
   push them. An epic gets its tag only once its doc is merged; a published tag
   is never moved.
8. **Release notes.** Publish them from the `v1.1.0` tag, stating that the
   numbers come from the idle-machine run and that Docker figures are
   functional results, not latency results.

## Key decisions and reasoning

- **One release for three epics.** The ring, the bindings and the order types
  share one contract (the Phase 2 definitions of the engine and of the tooling)
  and one DoD command list. Tagging them separately would create three
  release commits whose differences nobody can reconstruct; v1.1.0 is the
  Phase 2 checkpoint the roadmap already names (decision C56).
- **TSan is a required check, not an occasional run.** The ring is the one
  component whose correctness is a memory-ordering argument. Running the whole
  suite under ThreadSanitizer on every pull request is the only way that
  argument stays tested after the author moves on.
- **The stress binary is the volume evidence.** ThreadSanitizer finds races it
  observes; `spsc_stress` at 100 million messages finds ordering and content
  bugs by construction. The two are complementary and both are part of the DoD.
- **The binding compiles the real engine.** A Python reimplementation would
  need its own differential test and would make a mismatch indistinguishable
  from a Python bug. `pylob` compiles `src/order_book.cpp`,
  `reference/reference_book.cpp` and the flow-file reader, so its digests are
  the C++ digests.
- **The Phase 2 order types needed a contract first.** `types.hpp` is frozen
  and `OrderType` is a flow-file byte; the interim step-0 reject let the enum,
  the flow format and the wire protocol be fixed before either engine changed,
  so the two implementations could land in parallel (Epic 06).
- **Release numbers come only from an idle machine.** The same rule as Phase 1
  (C42): a latency figure produced while CI, builds or other agents share the
  machine is not a property of the code, and publishing it would make the
  release notes wrong in a way a reader cannot detect.
- **The Linux and Docker jobs became required after a green streak.** Advisory
  jobs that are green are evidence; requiring them immediately would have
  blocked every merge on a port that had not yet reported (the same ordering as
  D-A1 for the first required checks).

## Interfaces for future epics

- **`SpscRing` is the Phase 3 handoff.** The gateway uses three instances
  (commands, responses, market data) and `spsc_latency` is the harness that
  measures the ring plus the engine callback it carries. The type is
  header-only and needs no library target.
- **`pylob` is the scripting surface.** `replay(..., engine="reference")`
  compares the two engines without a C++ tool, and the flow reader plus
  `OrderBook` is the per-record path. A future published distribution starts
  from `python/pylob`.
- **FOK and post-only are values, not a new API.** The engine's public methods
  and listener did not change; downstream code that switches on `OrderType`
  needs the two new cases, and the wire protocol already accepts values 3 and 4.
- **The all-types mix is composable.** `all_types_mix(wide_mix())` and
  `--types all` work with every preset, so a future workload can opt into the
  Phase 2 types without a new generator.
- **The release checklist is reusable.** The Phase 3 release (Epic 10 and
  v2.0.0) follows the same shape: green required checks, merged docs, one
  idle-machine run, fresh-clone audit, tags, notes.

## Deviations

- **The release is cut by the orchestrator's script, not by hand.** Seed
  decision 12 described tags created by the human; C60 moves the mechanical
  tagging into the release script. The tag content and annotated-message rule
  are unchanged.
- **The ring latency harness is macOS-only.** `tools/spsc/latency.cpp` includes
  the benchmark timer (`bench/timer.hpp`), so the Linux build and the Docker
  image set `LOB_BUILD_BENCH=OFF` and skip both the benchmark binary and this
  harness. `spsc_stress` and the ring unit tests do build everywhere.
- **Python bindings are built and tested on macOS only.** The `test` CI job
  runs `uv sync` and the pytest suite on the macOS runner; the Linux job covers
  the C++ tree. A Linux wheel is not part of Phase 2.
- **The TSan evidence is the full suite, not a ring-only filter.** The audit
  script's `ctest -R -i ring` was invalid; the required `tsan` job's full-suite
  run is the stronger evidence, and it is what the release relies on.
- **The latency placeholders may still be marked pending when this doc
  merges.** The numbers are filled from the idle-machine run, which happens
  after the wave-08 audit. Publishing the doc before the run is deliberate: the
  checklist must exist before the release, not after it.
- **No `v1.1.0` tag exists at merge time.** The tag is the last step of the
  checklist; this document describes the release process and the verification
  it requires.

## Known limitations

- **The ring is SPSC only.** One producer and one consumer, a compile-time
  capacity and trivially copyable elements. A multi-producer design would need
  a different primitive; nothing in Phase 3 does.
- **The ring's capacity is fixed at compile time.** A full ring reports failure
  instead of growing, so each user has to choose a depth and handle `false`.
  The gateway's three depths (16,384 commands, 65,536 responses and market-data
  frames) are sized for the demo, not tuned for a production feed.
- **The binding holds the GIL and has no async API.** A long replay blocks
  other Python threads, and every listener event is a Python call. It is a
  tool for experiments, not a latency path.
- **The binding has no sdist, no type stubs and no abi3 wheel.** It is built
  from a repository checkout for the interpreter it is installed into.
- **FOK and post-only are limit-only.** There is no market FOK and no
  post-only-plus-time-in-force combination; a modified post-only order becomes a
  limit order (Epic 06).
- **Published latency numbers are still pending.** Until the idle-machine run
  exists, the README carries marked placeholders. This is a deliberate
  limitation, not a missing figure to be filled from a noisy run.
- **The nightly all-types runs are not a required check.** The required
  differential is one million operations; the 10-million-operation runs and the
  five-seed matrix happen nightly, so a rare mismatch can surface a day after
  the change that caused it.
- **macOS is the only platform with the benchmark and latency harnesses.** The
  Linux port covers the engine, tools, protocol and services, not the timing
  code, whose counter access is macOS-specific.
