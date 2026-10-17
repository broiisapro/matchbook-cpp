# Epic 00 — foundation

## Goal

Epic 0 lands everything the engine work needs before a line of matching logic
exists: a reproducible build with pinned dependencies, CI that runs the full
check set on this machine, the frozen shared interfaces both engines and every
tool build against, the flow-file wire format, and the committed architecture
document the rest of the project is measured against.

## How it works

### Build system

CMake 3.28+ with `CMakePresets.json` version 8, Ninja generator, C++20, Apple
clang from Xcode. Four configure presets exist, each with a matching build and
test preset:

- `release` — `Release` with `-O3 -DNDEBUG`.
- `debug` — `Debug`.
- `asan` — `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`,
  applied to compile and link; the test preset sets
  `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- `tsan` — `-O1 -g -fsanitize=thread`.

Binary directories are `build/<preset>`; `CMAKE_EXPORT_COMPILE_COMMANDS` is on.
No `-march`/`-mcpu=native` anywhere, so numbers and artifacts are reproducible
across Apple Silicon machines. Every target compiles with
`-Wall -Wextra -Wpedantic -Werror`; dependency headers come from imported
targets and stay outside the warning regime. Test, bench, reference, opgen and
flowfile sources are collected with `file(GLOB ... CONFIGURE_DEPENDS)`, so new
files never require editing `CMakeLists.txt`.

Targets: `lob` (fast engine, static), `lob_reference` (naive engine, static,
test-only), `lob_flowfile`, `lob_opgen`, `lobsim` (CLI), `lob_bench`
(benchmark binary), `lob_tests` and `test_alloc` (test binaries;
`test_alloc` is separate because it replaces the global allocation operators).

### Dependency pin

Dependencies come from vcpkg in manifest mode. `vcpkg.json` pins
`builtin-baseline` to the vcpkg release 2026.07.29 commit; the manifest
resolves `gtest` 1.17.0, `benchmark` 1.9.5, `hdr-histogram` 0.11.10 and
`xxhash` 0.8.3, target triplet `arm64-osx`. `scripts/bootstrap_vcpkg.sh`
clones microsoft/vcpkg in full (a shallow clone cannot resolve the baseline)
and checks out the same commit; when `VCPKG_ROOT` is set it verifies the clone
there and symlinks `./vcpkg` to it, which lets CI and worktrees share one
clone. The script takes a `mkdir` lock so parallel jobs can run it
concurrently, and a second run is a fast no-op. The `benchmark` port needs
`pkg-config` at install time; that is the only host prerequisite beyond Xcode,
CMake, Ninja and Homebrew.

### CI

Two workflows on self-hosted macOS ARM64 runners, per-branch concurrency with
cancel-in-progress and a 45-minute timeout per job:

- `build` — release configure and build.
- `test` — release configure, build and `ctest`.
- `asan` — asan configure, build and `ctest`.
- `format` — the pinned formatter version, then a dry-run check over all
  tracked sources.
- `tidy` — advisory clang-tidy from the Homebrew LLVM toolchain against the
  compile database; version drift keeps it non-blocking.
- `hygiene` — checks PR text, commit messages and commit identity against the
  patterns in repository variables.

Benchmarks are never a CI check. The `vcpkg` binary cache lives under the
runner user's home directory, so ports build once and are restored afterwards.

### Frozen interfaces

Everything in `include/lob/` plus the reference header, the flow record header
and the recording listener is frozen after this epic; later epics add
implementation behind them, never change signatures. See the section below for
what each header guarantees.

## Key decisions and reasoning

- **One commit pins the toolchain and the manifest.** The bootstrap script and
  `builtin-baseline` name the same vcpkg commit, so the tools and the resolved
  ports can never drift apart.
- **The listener is a virtual interface, not a template.** Both engines emit
  the same events through the same abstract class; a virtual call per event is
  a few nanoseconds and shows up in measurements instead of being hidden by
  inlining.
- **The digest builder only hashes bytes.** Each engine walks its own
  structures and streams levels and orders into the builder, which serializes
  little-endian byte by byte and hashes with XXH64. Sharing the builder does
  not make the differential test circular, and the golden values in the digest
  tests were computed outside C++ so the byte layout is independently pinned.
- **The reference engine shares only headers.** `lob_reference` includes
  only `types.hpp`, `config.hpp`, `listener.hpp` and `digest.hpp`; the
  reference header even forward-declares `LevelInfo` so it never includes the
  fast engine's header, while `top_levels` keeps the identical signature.
  Member-signature static assertions enforce the mirror at compile time.
- **`test_alloc` is its own binary.** It will replace the global allocation
  operators, which must not affect the unit-test binary.
- **Sanitizer presets leave the build type empty and pass flags directly.**
  With a single-config generator this guarantees `-O1 -g` plus the sanitizer
  flags apply to every translation unit and the link step.
- **Fixed flow-file records.** A 32-byte header and 40-byte little-endian
  records make files seekable and versionable; the record struct is logical
  only (no `memcpy` of structs anywhere, so padding and host endianness cannot
  leak into the format).
- **`[[maybe_unused]]` on the frozen private members.** The stubs do not read
  `config_` or `listener_` yet, and `-Wunused-private-field` is an error; the
  attribute disappears when the Epic 1 implementations read the members.

## Interfaces for future epics

- `include/lob/types.hpp` — `Price` (i64), `Qty` (i64), `OrderId` (u64),
  `Side`, `OrderType`, `RejectReason` with values fixed for the wire formats;
  `opposite()` and `to_string()` helpers.
- `include/lob/config.hpp` — `Config { min_price, max_price, max_orders,
  max_levels }` and `valid()`; both engines take the same Config and must
  enforce the same limits.
- `include/lob/listener.hpp` — `Listener` with six callbacks and
  `NullListener`. Callbacks are synchronous, ordered by the event contract,
  and must not call back into the book.
- `include/lob/digest.hpp` — `DigestBuilder`: version byte, then levels best
  first per side and orders in FIFO order, little-endian; `finish()` returns
  the XXH64 digest with seed 0. Golden values lock the byte stream.
- `include/lob/order_book.hpp` — `OrderBook` and `LevelInfo`: submit, cancel,
  modify, best_bid, best_ask, level_qty, top_levels, order_count, digest,
  non-copyable and non-movable, with the event sequence of each method
  documented in the header.
- `reference/reference_book.hpp` — `lob::ref::ReferenceBook`, the same API for
  the differential harness, deliberately naive and never optimized.
- `tools/flowfile/flow_record.hpp` — `FlowRecord`, `FlowHeader`, `FlowOp`,
  the four format constants, `encode`/`decode` and `encode_header`/
  `decode_header`; decode of the header rejects a bad magic, version or record
  size.
- `tests/support/recording_listener.hpp` — comparable, printable `Event`
  variant and `RecordingListener` shared by the engine tests and the
  differential test.

## Deviations from the plan

- The workflows use `actions/checkout@v7`; v4 was the referenced major in the
  plan but v7 is current and was verified against the release API.
- `lob_flowfile` and `lob_opgen` publish the engine's include directories
  instead of linking `lob`, and every executable links `lob` directly. This
  keeps duplicate static archives off the link line.
- `tests/test_smoke.cpp` was removed: `tests/test_interfaces.cpp` covers its
  purpose.
- The architecture document's digest section mentions a `begin()` method; the
  frozen builder writes the version byte in its constructor instead, which is
  what the interface specification for this epic requires. The document text
  is kept verbatim.
- The reserved flow-header field is written as zero and ignored when decoding,
  rather than validated.

## Known limitations and deferred items

- Both engines are stubs: no matching, no pools, no digest walk over real
  levels. Epic 1 fills them in without changing the frozen interfaces.
- `test_alloc` contains only a placeholder; the real allocation counting lands
  with the engine.
- The benchmark binary runs one trivial benchmark; the suites and the HDR
  latency harness arrive with Epic 3.
- Flow files cannot be read or written yet; `flow_record.hpp` defines the
  format and encode/decode only.
- `lobsim` subcommands print "not implemented" and exit 2.
- No Tardis fixture, no operation generator and no Python report scripts yet.
- `README.md` is not written yet.
- Required status checks on the `main` ruleset are added once the CI workflow
  has reported on a pull request.
- Leak detection is disabled in the asan test preset: the sanitizer
  interceptors on macOS make leak reports unreliable in this setup.
