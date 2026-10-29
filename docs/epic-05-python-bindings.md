# Epic 05 — Python bindings

## Goal

The engine is a C++ library, but the people who want to poke at a limit order
book — reviewers, quants, anyone writing a small analysis script — work in
Python. Rewriting matching logic in Python would defeat the point: the interest
is in the real engine, with its real event order and its real book digest.

Epic 05 exposes the existing C++ implementation to Python without adding a
second implementation or a new C++ API. A Python program can build a book,
submit/cancel/modify orders, receive the same events the C++ listener receives,
inspect the book, replay a flow file through either engine, and compare digests.
The Phase 2 definition of done — `uv sync --frozen && uv run python -c "import
pylob"` from the repository root — works from a clean checkout, and the spread
example in `examples/` replays a flow file through the binding and plots it.

## How it works

### Packaging: scikit-build-core and pybind11

`python/pylob/` is its own Python project:

- `pyproject.toml` uses `scikit-build-core>=0.11` as the build backend and
  `pybind11>=2.13` as a build requirement, so the extension is compiled in an
  isolated build environment with the pybind11 headers and CMake config from
  PyPI rather than from the repository's vcpkg tree. `requires-python` is
  `>=3.12` (the pinned interpreter), the package version is 0.1.0, and
  `wheel.packages = ["src/pylob"]` marks the pure-Python part of the package.
- `CMakeLists.txt` is a standalone CMake project, not a subproject of the
  repository root. scikit-build-core configures it inside the isolated build
  environment, so it must not depend on vcpkg or on the root `CMakeLists.txt`.
  It compiles the engine sources the bindings need — `src/order_book.cpp`,
  `reference/reference_book.cpp` and `tools/flowfile/flowfile.cpp` — into a
  static `pylob_engine` with `LOB_PRICE_LADDER_ARRAY=1`, exactly the ladder the
  release presets use, then builds `pybind11_add_module(_core …)` against it and
  installs `_core` into the `pylob` package directory.
- xxHash is resolved at configure time: `find_package(xxHash CONFIG QUIET)`
  reuses a package when the build happens to see one (vcpkg, Homebrew), and
  otherwise the pinned 0.8.3 release tarball is fetched by URL and SHA-256 and
  `xxhash.c` is compiled directly into a small static library. Compiling the
  single C source avoids vendoring code and keeps the upstream install rules
  (library, header, pkg-config file) out of the wheel. A cold build therefore
  needs network access, like the PyPI build requirements do.

### The uv workspace

The root `pyproject.toml` is a uv workspace whose only member is
`python/pylob`; it also declares `pylob` as a dependency of the repository
environment through `[tool.uv.sources]`, so a plain `uv sync --frozen` at the
root builds and installs the extension and `uv run …` can import it. The
development dependency group carries pytest; a separate `plot` group carries
matplotlib and is in `default-groups`, so the example runs with a plain
`uv run` while matplotlib stays out of `pylob`'s own dependencies. `package =
false` keeps the root project from being installed as a package, and `uv.lock`
is committed so CI is reproducible.

### What is exposed

Everything below is a binding of existing C++ functionality; no matching
behaviour is reimplemented in the binding:

- **Enums.** `pylob.Side` (`Buy`, `Sell`), `pylob.OrderType` (`Limit`,
  `Market`, `IOC`, `FOK`, `PostOnly`) and `pylob.RejectReason` (`DuplicateId`,
  `UnknownOrder`, `InvalidQuantity`, `InvalidPrice`, `CapacityExceeded`,
  `ModifyNoChange`, `UnsupportedOrderType`, `WouldCross`). The member names are
  the CamelCase form of `lob::to_string` words — `fok`, `post_only`,
  `would_cross` — which are also the words the CSV flow format uses, and the
  integer values are the frozen flow-file bytes.
- **`Config`.** The same four fields as `lob::Config` (price range, order and
  level capacities) with the flow tools' defaults, plus a readable `__repr__`.
  Construction of an `OrderBook` validates the configuration (`ValueError` for
  an unusable one).
- **`LevelInfo`** and **`OrderBook`.** `OrderBook(config, listener=None)` owns
  the C++ book and the listener bridge, and exposes `submit`, `cancel`,
  `modify`, `best_bid`, `best_ask`, `level_qty`, `top_levels`, `order_count`
  and `digest` with the same semantics and validation order as the C++ API.
  `top_levels` returns `LevelInfo` objects best-first, and `best_bid`/`best_ask`
  return `None` for an empty side.
- **`read_flow(path)`.** A thin binding of the flow-file reader: binary by
  default, CSV when the path ends in `.csv`, returning immutable record objects
  with `op` (`"submit"`, `"cancel"`, `"modify"`), side, type, id, price, qty and
  `ts_ns`. The whole file is loaded, so this is for tools and examples rather
  than huge flows.
- **`replay(path, engine="fast", config=None)`.** Replays a flow through
  `lob::OrderBook` or `lob::ref::ReferenceBook` and returns a dictionary with
  the same fields `lobsim replay` prints: `records`, `submits`, `cancels`,
  `modifies`, `accepts`, `rejects`, `trades`, `traded_qty`, `final_orders`,
  `digest`, `wall_ms`, `ns_per_record`. Binary and CSV flows are both accepted,
  and the configuration defaults to the flow tools' default replay config. The
  reference engine is included so a script can compare the two engines' summary
  and digest without a C++ tool.

### Events and the listener

`pylob._events` provides `Listener`, an empty base class with one no-op method
per event, and `RecordingListener`, which collects every event in order.
Events are frozen, slotted dataclasses with value equality and a `kind` class
attribute, which makes them pleasant to assert on:

```python
listener = pylob.RecordingListener()
book = pylob.OrderBook(pylob.Config(), listener)
book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 5)
book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 100, 5)
assert listener.events == [
    pylob.Accept(1),
    pylob.BookUpdate(pylob.Side.Sell, 100, 5),
    pylob.Accept(2),
    pylob.Trade(1, 2, 100, 5),
    pylob.BookUpdate(pylob.Side.Sell, 100, 0),
]
```

The bridge is deliberately duck-typed and cheap to cross once: the C++
`PythonListener` resolves each callback with `hasattr` at construction time,
keeps a reference to the Python object for as long as the book can call it, and
skips the callbacks an object does not define. Any object with the matching
method names works as a listener — `test_any_object_with_callbacks_works_as_a_listener`
uses a class that implements only `on_trade`.

### The slow Python listener path

A listener is optional. With `listener=None` the book is constructed with
`lob::NullListener`, so the no-listener path is the C++ engine's normal path
with a call that immediately returns. With a Python listener every event is a
Python call that holds the GIL: an interpreter call per event next to an engine
event that costs a few tens of nanoseconds, which changes the timing of the very
code being measured. The docstrings say so where a user will see it: the
`Listener` class documents that callbacks run inside
`submit`/`cancel`/`modify` on the calling thread with the GIL held, and the
`OrderBook` docstring repeats it.

Two consequences are documented rather than papered over:

- An exception raised by a callback propagates out of the `submit`/`cancel`/
  `modify` call. The operation is not rolled back, so the book may have changed
  partially; the alternative — swallowing the exception — would leave the
  caller without a failure signal.
- A callback must not call back into the book. The engine asserts this in debug
  builds (the `OrderBook` reentrancy guard), and the binding does not add an
  escape hatch.

### Replay and the spread example

`pylob.replay` is the scripting entry point: one call, a summary dictionary,
and the digest that makes two runs comparable. `pylob.read_flow` plus
`OrderBook` is the per-record path for tools that need state between records.
`examples/replay_spread.py` (from #94) is that second path in practice: it
replays a flow record by record, samples `best_bid()`/`best_ask()` every
`--every` records and on the last record, computes the spread, and writes a PNG
with matplotlib. The x axis is relative time in seconds when the flow's
timestamps advance and the record index otherwise; a flow that never has both
sides is a clean error. The example's test runs it as a subprocess on a
20,000-order flow and checks a valid PNG and more than 100 samples.

## Key decisions and reasoning

- **Bind the real engine instead of reimplementing in Python.** A second
  implementation would need its own differential test against the first, and a
  mismatch in the binding would be indistinguishable from a Python bug. The
  binding compiles the same engine, reference-engine and flow-file sources the
  C++ tools build.
- **Its own CMake project.** The root project builds every target with vcpkg,
  GoogleTest and the benchmark harness; a wheel must not inherit any of that.
  A standalone project with three sources and one dependency keeps the build
  isolated and the wheel minimal.
- **xxHash fetched at build time rather than vendored.** The engine's digest is
  XXH64, so the extension needs the library. Fetching the pinned release by
  SHA-256 avoids a vendored copy in the repository, and compiling `xxhash.c`
  directly avoids shipping upstream's install rules in the wheel.
- **A uv workspace member instead of a second lockfile.** `uv sync --frozen`
  from the root installs the bindings and the dev tools in one step, which is
  exactly the PRD command. Two environments would let the lockfiles drift.
- **matplotlib in an optional group, not a dependency.** The bindings stay
  dependency-free; the example is the only consumer. `default-groups` keeps the
  documented `uv run` command working without extra flags.
- **Duck-typed listeners.** Requiring a Python subclass would stop a user from
  passing a small object with one callback, which is the common case for
  experiments. Resolving the callbacks once means the per-event cost does not
  include repeated attribute lookups.
- **Return a plain dictionary from `replay`.** The summary fields mirror
  `lobsim replay`, so a script and the CLI can be compared directly, and no
  Python wrapper type has to be maintained alongside the C++ one.
- **Keep the version at 0.1.0.** The wheel is a workspace member built from a
  checkout; it does not track the engine's release tags, and claiming otherwise
  would imply a published artifact that does not exist yet.

## Interfaces for future epics

- **Wheel build path.** `python/pylob` is where a future published distribution
  starts; the CMake project and the xxHash resolution are already isolated from
  the repository build.
- **`replay(..., engine="reference")`.** The two-engine switch is the scripting
  equivalent of the differential test; later phases (all order types, gateway
  event streams) can compare summaries without a new tool. The all-types
  differential itself stays in C++ (#116).
- **Enum coverage is complete.** `OrderType` and `RejectReason` now bind every
  value in `lob/types.hpp`, including the Phase 2 `FOK`/`PostOnly` types and the
  `UnsupportedOrderType`/`WouldCross` reasons added with the Phase 2 contract
  (#90, #117). A new appended enum value needs one line in `bindings.cpp`.
- **Test pattern for state-dependent semantics.** The FOK/post-only tests probe
  what the engine implements (a probe submit, not a version constant) and skip
  the final-semantics assertions with a reason on an engine that still rejects
  the type. The same pattern fits any future feature that rolls out across
  engines at different times.
- **CI.** The required `test` job runs `uv sync --frozen` and the binding
  tests, so a change to the engine sources or the binding cannot land without
  the Python suite passing on the same commit.

## Deviations

- **Not publishable as a standalone sdist.** The wheel is built from a
  repository checkout: its CMake project compiles `../../src`,
  `../../reference` and `../../tools/flowfile`. An sdist would have to carry
  those sources and a build definition for them, which is deliberately out of
  scope; the package metadata points at the repository instead of a
  distribution page.
- **A cold build needs network access.** pybind11 and scikit-build-core come
  from PyPI in the isolated build environment, and if no xxHash package is
  visible the pinned tarball is downloaded. The repository build itself remains
  fully offline after vcpkg is bootstrapped; the wheel build is the exception.
- **No type stubs.** The extension has no `.pyi` file, so type checkers see the
  pybind11 module as untyped. Docstrings carry the contract.
- **No abi3 / limited API.** The wheel is built for the interpreter it is
  installed into (Python 3.12), not as a version-independent binary.
- **The example's matplotlib is a repository-environment dependency only.**
  `pylob` itself has no runtime dependencies; the `plot` group is installed by
  default for the workspace so the documented command works.
- **Enum members are not singletons.** A pybind11 enum value returned from C++
  (for example a `Reject.reason`) compares equal to the class member but is not
  the identical object, so the tests compare with `==`; this is a pybind11
  property, not a choice in this binding.
- **The version is independent of the engine.** `pylob.__version__` is 0.1.0;
  the engine's v1.0.0 tag predates the bindings' merge.

## Known limitations

- **One interpreter, one thread.** The GIL is held for the whole of a call, and
  a long replay blocks every other Python thread. There is no `nogil` release
  and no async API.
- **Python callbacks are slow.** A `RecordingListener` on a large replay turns
  a microsecond-scale operation into a Python call per event; the binding is for
  tools and experiments, not for latency measurement.
- **No exception safety for partially applied operations.** A callback that
  raises leaves the book as it was after the event that raised, not as it was
  before the call.
- **Replay is whole-file.** `pylob.replay` streams through the C++ reader but
  `read_flow` loads every record into a list; very large flows should be
  processed through the C++ tools or split.
- **No packaging for distribution.** The wheel is installable from the workspace
  but there is no sdist, no PyPI release and no job that builds wheels for
  another platform; CI builds and tests the extension on macOS only.
- **The reference engine is available only through `replay`.** There is no
  bound `ReferenceBook` class; scripts can compare summaries and digests, not
  drive a reference book step by step.
- **Not every C++ feature is bound.** `top_levels` takes a count and returns a
  list instead of writing into a caller-provided span, and the engine's debug
  invariant assertions are not reachable from Python. The binding covers the
  public behaviour, not the internals.
