# Epic 10 — the Compose demo

## Goal

The Phase 3 definition of done includes a demo a reviewer can run with three
commands: build one image, bring the stack up, push a million orders through it,
and look at the report. That is a different kind of evidence from a unit test.
It proves that the four services work together on a real network stack, that the
Linux port is not just compilable but runnable, and that the market-data path
delivers a complete, gap-free stream while the order path answers every order.

Epic 10 is that demo: a `linux/arm64` image built from the repository, a
Compose topology with health checks and startup ordering, the literal PRD
commands, and the CI jobs that keep both honest. The numbers from the demo are
**functional** — orders answered, gaps recovered — not a latency claim. The
published latency results come from the idle-machine run described in Epic 09.

## How it works

### The image

`docker/Dockerfile` is a multi-stage build for `linux/arm64`, with three stages:

1. **`vcpkg`** clones microsoft/vcpkg at the commit that is the
   `builtin-baseline` in `vcpkg.json` (`9e593bb18ea69cc5095e012465dcd675a822ed0d`)
   in its own stage, so editing the source does not re-clone the toolchain.
2. **`build`** is Ubuntu 24.04 with clang, CMake, Ninja, pkg-config and the
   vcpkg tree. It configures the project directly rather than through
   `CMakePresets.json` (the presets pin the macOS triplet and toolchain paths)
   with the `arm64-linux` triplet, `LOB_BUILD_BENCH=OFF` and a flattened runtime
   output directory, builds the default `all` target — the whole portable tree,
   including the four services — and then runs the full CTest suite in the
   image. A broken Linux port fails the image instead of shipping a binary
   nobody exercised.
3. **`runtime`** is Ubuntu 24.04 plus `libstdc++6`. It contains `lobsim`,
   `lob_gateway`, `lob_publisher`, `lob_md_listen`, `lob_loadgen` and the
   `loadgen-entrypoint` script; its default entrypoint is `lobsim`, so the
   existing `docker run <image> difftest …` usage still works.

The benchmark binary and the SPSC latency harness are deliberately absent: both
include the macOS timer in `bench/timer.hpp`, and `LOB_BUILD_BENCH=OFF` removes
them. `bash` is present in the Ubuntu base image and is what the health checks
use, so the runtime installs nothing beyond the C++ runtime.

`scripts/docker_build.sh` builds the image and then smoke-tests the result: it
runs `lobsim --help`, a 100,000-operation differential test and the help text of
the four services inside the container. Like the Compose script, it routes the
build through the machine's heavy-command slot gate when one is present; a plain
clone runs Docker directly.

### The Compose topology

`docker-compose.yml` runs five services on a private bridge network
(`10.213.77.0/24`). There are no published ports and no host networking, so the
demo cannot collide with a port already in use on the host.

| Service | Image / command | Address | Role |
|---|---|---|---|
| `gateway` | `lob_gateway --port 9000 --md-port 9001` | 10.213.77.10 | order entry and market-data event stream |
| `publisher` | `lob_publisher --gateway gateway:9001 --recovery-port 9002` | 10.213.77.11 | UDP fan-out to `10.213.77.12:9100`, `10.213.77.13:9101` and `127.0.0.1:9100`; recovery on 9002 |
| `client1` | `lob_md_listen --udp-port 9100 --recovery publisher:9002 --top 5` | 10.213.77.12 | first listener |
| `client2` | `lob_md_listen --udp-port 9101 --recovery publisher:9002 --top 5` | 10.213.77.13 | second listener |
| `loadgen` | one-off, `profiles: ["load"]` | shares the publisher's namespace | the PRD load run |

The two listener addresses are static because the publisher resolves and caches
its UDP destinations when it starts, before Compose has created those
containers; service names would not resolve yet. Static addresses are routable
on the bridge from the first packet.

The load generator is a one-off, started only by `docker compose run`, not by
`up`. Its `loadgen-entrypoint.sh` fills in `--gateway gateway:9000`,
`--md-udp-port 9100` and `--recovery 127.0.0.1:9002` when the caller did not pass
them, so the PRD's short command works unchanged. Its market-data monitor binds
loopback, so the service shares the publisher's network namespace and the
publisher has an extra `127.0.0.1:9100` destination for it; order entry still
crosses the bridge to `gateway:9000`. The report volume `loadgen-out:/out`
survives `down`, which is how `scripts/compose_demo.sh` reads the report after
the stack is gone.

### Health checks and startup ordering

Every long-running service has a health check, and every dependent starts only
after its dependency is healthy:

- **gateway** — opens `/dev/tcp/127.0.0.1/9000` and
  `/dev/tcp/127.0.0.1/9001`, proving both listeners accept a connection.
- **publisher** — greps `/proc/net/tcp` for an established connection to the
  gateway's event-stream port (`:2329` = 9001) and for its recovery listener
  (`:232A` = 9002).
- **client1 / client2** — greps `/proc/net/udp` for a bound socket on 9100
  (`:238C`) and 9101 (`:238D`).

The timing is shared: 2 s interval, 2 s timeout, 30 retries and a 2 s start
period. `depends_on` uses `condition: service_healthy`, so the publisher starts
after the gateway is serving, the listeners after the publisher is connected,
and the load generator after both the gateway and the publisher are healthy.
`restart: unless-stopped` keeps the long-running services up, and a 5 s
`stop_grace_period` gives them time to drain on `down`. The listeners run under
`stdbuf -oL` so their periodic top-of-book lines appear in `docker compose logs`
instead of sitting in a block buffer.

### The PRD Phase-3 commands

The demo runs exactly the three commands from the definition of done:

```sh
docker compose up -d --build
docker compose run --rm loadgen --orders 1000000 --report /out/w2w.json
docker compose down
```

`scripts/compose_demo.sh` runs those commands, prints the report and, when
`python3` is available, checks that the market-data stream had no unrecovered
sequence gap. It accepts `--orders N`, supports `--keep` to leave the stack
running, and cleans up through an `EXIT` trap so a failed run does not leave
containers behind. The image build and the load run go through the
heavy-command slot gate when one is available; `down` does not, so a failed run
cannot hold a slot while it tears down.

### The verified run

The demo was run end to end on 2026-10-28 with the literal PRD commands and
1,000,000 orders. The functional counters from the report:

| Counter | Value |
|---|---|
| orders requested / sent | 1,000,000 / 1,000,000 |
| orders without a response | 0 |
| market-data frames received | 1,647,062 |
| gaps detected / recovered | 0 / 0 |
| sequence resets | 0 |
| final published sequence | 1,647,062 |
| unrecovered sequence gaps | 0 |
| dropped / malformed frames | 0 |

The stack reached healthy, the load generator completed, the report check
passed, and `compose down` removed everything. Both listener logs showed the
same top-of-book state. This is a **functional** result: it proves that every
order was answered and every market-data number was accounted for. The run also
records latency percentiles, but those come from a shared Mac running other
containers and CI, so they are not published as latency results — the README's
numbers come only from the idle-machine run (Epic 09), and Docker figures are
never presented as latency claims.

The market-data counters cover the run window: the load generator's monitor
stops with the last order, so datagrams published after the final response are
not counted. A gap inside the window would still have been detected and
reported, and the idle heartbeats make a lost final datagram visible in the
native wiring test.

### Linux and Docker CI

Two required jobs keep the image and the port from rotting:

- **`linux`** runs on `ubuntu-24.04-arm`. It configures the
  `linux-release` preset (which sets `LOB_BUILD_BENCH=OFF`), builds the default
  `all` target — the whole portable tree, so a new executable is picked up
  without a hand-maintained list — and runs the full CTest suite, including the
  end-to-end wiring test now that the image and the job build all four services.
- **`docker`** runs on `ubuntu-24.04-arm` too. On a pull request it builds the
  image only when the change touches a path the image is made of (`docker/*`,
  `src/*`, `include/*`, `net/*`, `apps/*`, `tools/*`, `reference/*`, `tests/*`,
  the root `CMakeLists.txt`, `vcpkg.json`, the CI workflow or the setup
  actions); a push to `main` always builds. The build uses BuildKit with a
  GitHub Actions cache (`mode=max`), then runs `lobsim --help` and a
  100,000-operation differential test inside the image.

Both were advisory while the port was new and became required after a green
streak on `main` (decision C55). Together they mean the Compose demo's binaries
are the ones CI built and tested, not a local one-off.

## Key decisions and reasoning

- **One image for every service.** The gateway, publisher, listener and load
  generator are built from the same source and the same vcpkg tree; separate
  images would multiply the build cost and let their protocol versions drift.
  The runtime image is shared and the command differs per service.
- **Build the tree directly, not through the presets.** `CMakePresets.json`
  pins the macOS triplet and toolchain paths. A Linux image needs a different
  triplet and flattened runtime output; keeping that in the Dockerfile makes
  the difference explicit instead of teaching the presets about a platform they
  are not used for.
- **Run the tests in the build stage.** A Linux port that compiles but does not
  pass its tests is not a port. The image build is the only place where the
  full suite runs on arm64 Linux for every source change, and a failure stops
  the image rather than shipping it.
- **No benchmark in the image.** The benchmark and the SPSC latency harness use
  `mach_absolute_time`, and Docker numbers are never published as latency. There
  is no reason to carry a binary whose only output would be misused.
- **Static listener addresses and explicit destinations.** The publisher
  resolves destinations once at startup, before Compose creates the listeners.
  Static addresses make the demo deterministic and work from the first packet;
  service names would race container creation.
- **Health checks that test the actual dependency.** The gateway check opens
  both listening ports; the publisher checks its established event-stream
  connection and its recovery listener; the listeners check their bound UDP
  sockets. Checking that a process is running would not prove the wiring, and
  `depends_on: service_started` would start the publisher before the gateway
  was accepting.
- **A one-off load generator with an entrypoint that fills defaults.** The PRD
  command is deliberately short. Putting the demo endpoints in the Compose
  environment and filling them in the entrypoint keeps that command literal
  while an explicit flag on the command line still wins.
- **The load generator shares the publisher's namespace.** Its market-data
  monitor binds loopback and has no bind-address flag yet. Rather than change
  another workstream's component for the demo, the one-off shares the
  publisher's namespace and the publisher sends it a loopback copy; order entry
  still crosses the bridge. The workaround is documented and has a follow-up.
- **Functional numbers here, latency numbers elsewhere.** A container run on a
  shared Mac measures the machine's contention as much as the code. The demo's
  value is completeness — every order answered, every sequence accounted for —
  and the doc states that boundary rather than blurring it.
- **Required Linux and Docker jobs after a green streak.** They were advisory
  while the port was new; promoting them once they had reported green makes the
  port a merge gate without blocking the wave that created it.

## Interfaces for future epics

- **The image is the deployment artifact.** A future release can publish the
  same `linux/arm64` image; the runtime stage already contains every service and
  the default entrypoint is `lobsim`.
- **Compose is the topology description.** New services join the bridge with a
  static address, a health check and a `depends_on` condition; the demo script
  and the health checks are the template.
- **The PRD commands are the contract.** `docker compose up -d --build`,
  `docker compose run --rm loadgen …` and `docker compose down` must keep
  working; the entrypoint fills defaults so the command stays short.
- **`scripts/compose_demo.sh` is the automated form.** It adds the heavy-slot
  gate, `--orders`, `--keep`, the report check and cleanup, and is what a
  release audit runs instead of typing the three commands by hand.
- **CI cache scopes are named.** The Docker job's `matchbook-docker` buildx
  cache and the macOS jobs' ccache/vcpkg caches are independent, so a future
  image change does not invalidate the compiler cache and vice versa.
- **Functional and latency evidence stay separate.** The Compose run and the
  wiring test produce completeness counters; `scripts/run_phase23_benchmarks.sh`
  on an idle machine produces the published latency numbers. A future report
  should keep that split.

## Deviations

- **The image is `linux/arm64` only.** It targets the same Apple Silicon class
  of machine as the development environment and the hosted arm64 runners. A
  multi-arch image would need a second vcpkg triplet build and is out of scope.
- **No published ports.** The demo is deliberately host-network-free, so the
  three PRD commands work without a port already in use on the host. Reaching
  the gateway from the host requires adding a `ports:` entry, which the Compose
  file documents in a comment.
- **The load generator uses the publisher's network namespace.** Its monitor
  binds loopback and has no bind-address flag in this version; the workaround is
  visible in Compose and named as a follow-up rather than hidden.
- **The demo's latency figures are not published.** They are recorded in the
  report and visible to anyone who runs the demo, but the documented latency
  results come from the idle-machine run. A container run on a shared machine is
  not a measurement of the code.
- **Health checks use `/dev/tcp` and `/proc/net`.** They rely on bash and Linux
  procfs inside the container, not on a portable probe. The runtime image is
  Ubuntu, which has both; the checks are not intended to run on macOS.
- **The Docker CI job skips some pull requests.** It path-filters on the files
  the image is made of, so a docs-only pull request does not rebuild a large
  image. A push to `main` always builds, which keeps the "green on main" claim
  true.
- **The `loadgen-entrypoint` is a shell script.** It fills three flags and
  `exec`s the binary; a C++ wrapper would add a build target for no behaviour.

## Known limitations

- **The demo is single-instrument.** One gateway owns one `OrderBook`; there is
  no symbol routing and no second instrument in the topology.
- **No authentication or TLS.** The stack is a loopback/private-bridge demo, not
  an internet-facing service.
- **No snapshot for late subscribers.** A listener that starts after the
  publisher begins builds from the current sequence; the listeners start before
  the load, which is why the demo sees a complete book.
- **The retransmit window is finite.** The publisher keeps 100,000 messages in
  the demo (65,536 by default). A subscriber that falls further behind gets a
  `SequenceReset` and must rebuild from live traffic.
- **The load generator's monitor stops with the last order.** Market-data
  counters cover the run window, not the moments after the final response; a
  gap there is caught by the heartbeats in the native wiring test, not by the
  demo report.
- **The demo has no latency claim.** Its percentiles are container measurements
  on a shared machine; publishing them would misrepresent the software.
- **One image platform.** `linux/arm64` only; an amd64 image would need its own
  vcpkg triplet and CI runner.
- **The Compose topology is not a production deployment.** `restart:
  unless-stopped` and health checks keep the demo observable, but there is no
  orchestration, resource limit or rolling update.
