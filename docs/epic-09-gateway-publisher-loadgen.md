# Epic 09 — gateway, publisher and load generator

## Goal

Phase 3 turns the single process into an exchange-shaped stack. A **gateway**
owns the book and speaks order entry and market data over TCP; a **publisher**
fans the market-data stream out over UDP and answers recovery requests; a
**feed handler** rebuilds the book from that stream and repairs gaps; a **load
generator** drives the whole thing and measures what a client experiences.

Epic 09 is the process model and the three services around the engine. It does
not change matching: the gateway runs the same `OrderBook`, the same listener
and the same event contract as `lobsim`. What it adds is the wiring that makes
those events observable by another process — two threads and three rings in the
gateway, a gap-free sequence number on every market-data frame, a UDP hop with a
retransmit store, and a client whose closed loop defines exactly what
"wire-to-wire" means here.

The design follows decision C54: one connection is one session, the engine order
id is `(session_id << 40) | client_order_id`, and responses carry the client's
own id back.

## How it works

### The process model

| Process | Executable | Sockets | Threads |
|---|---|---|---|
| Gateway | `lob_gateway` | order entry TCP 9000; market-data stream TCP 9001 | one network thread, one engine thread |
| Publisher | `lob_publisher` | connects to 9001; UDP fan-out; recovery TCP 9002 | one poll loop |
| Feed handler | `lob_md_listen` | UDP receive 9100; recovery client to 9002 | one thread |
| Load generator | `lob_loadgen` | connects to 9000; optional UDP monitor and recovery | one send loop, plus a monitor thread when it counts market data |

Every port is a flag and the gateway accepts 0, reporting the port the kernel
chose; the Compose demo and the wiring test rely on that. The libraries live in
`net/<component>/` (`gateway`, `publisher`, `md_client`, `loadgen`) and are
compiled into `lob_net`; the executables are thin `main.cpp` files under
`apps/<name>/` (decision C52). The four services, `lobsim` and the engines all
speak the §14 protocol from `lob_net`.

### Gateway threads and rings

The gateway runs exactly two threads. The **network thread** owns every socket,
accepts connections, frames input with one `StreamFramer` per connection, writes
order-entry responses and fans market data out to subscribers. The **engine
thread** owns the single instrument's `OrderBook`, the only `Listener`, and
applies every command in arrival order.

They exchange three fixed-size SPSC rings:

| Ring | Direction | Depth | Element |
|---|---|---|---|
| Commands | network → engine | 16,384 | one decoded NewOrder, Cancel or Modify |
| Responses | engine → network | 65,536 | one encoded order-entry frame plus its target session |
| Market data | engine → network | 65,536 | one encoded BookUpdate or Trade frame |

The engine thread allocates nothing per message: commands are fixed-size
values, frames are encoded into stack buffers, and the rings are allocated once
with the gateway. Apart from the rings, the threads share only the atomic
counters exposed as `md_sequence_published`, `md_sequence_forwarded`,
`subscriber_count` and `session_count`, and a self-pipe used to wake the network
thread; the engine writes at most one byte while a wake-up is pending.

Backpressure is one-directional and bounded. The network thread only hands
buffered bytes to the framer when the command ring has room for every frame that
buffer can contain, so it never waits on the engine. A client that runs ahead of
the book is left with its bytes buffered (1 MiB per connection) and is
disconnected past that bound. The engine never drops an event: if the
market-data ring is full it waits for the network thread to drain it, and a
subscriber that cannot keep up is disconnected instead of slowing the engine.

The gateway also bounds the number of peers: at most 1024 concurrent order-entry
sessions and 256 concurrent market-data subscribers. A connection arriving
beyond either limit is closed immediately, and responses queued for one session
are capped at 4 MiB so a client that stops reading is disconnected rather than
growing the gateway.

### Sessions and order ids

One accepted connection is one session. v2.0 has no logon, authentication or
session-resume message, so the session begins at connect and ends at close.
Session ids start at 1, are never reused and fit in 24 bits; at most 1024
sessions exist at once.

The engine order id is `(session_id << 40) | client_order_id`; a client order
id must fit in 40 bits. An id that does not is answered with `Reject` and
`RejectReason::CapacityExceeded` and leaves the session open. A session's
resting orders stay in the book after it disconnects: there is no mass-cancel or
session-cancel message in v2.0.

Order-entry responses (`Ack`, `Reject`, `Fill`, `Cancelled`) go to the session
that owns the order and carry **client** order ids — the low 40 bits of the
engine id, which is what the client sent. A `Fill` is sent to the maker's
session and to the taker's session (once when both are the same), with the maker
and taker fields both expressed as client ids. Market-data messages carry the
full engine order ids because a publisher or feed handler sees several sessions
at once.

On order-entry connections the header `seq` is the gateway's per-connection
counter: it starts at 1 and increases by one for every frame written on that
connection. The network thread stamps it when it appends the frame, so the
counter has a single owner. Because §14 has no `Modified` message, an in-place
modify (same price, reduced quantity) is acknowledged with an `Ack` for the same
order id; a cancel+new modify emits `Cancelled` followed by the normal submit
`Ack`.

### The market-data event stream

The engine thread publishes one message per `on_book_update` (`BookUpdate`) and
per `on_trade` (`Trade`), in the order the book produced them. The header `seq`
of those frames is a single, gap-free stream sequence starting at 1 — on this
stream it is a stream sequence, not a per-connection counter. The gateway serves
the stream on the market-data port to up to 256 subscribers at once.

A subscriber receives the stream from the moment its connection is accepted;
there is no snapshot in v2.0, so a late subscriber starts at the current
sequence and must build its book from later updates. Every subscriber gets the
same frames with the same sequence numbers.

Slow subscribers do not slow the engine: the gateway queues at most **1 MiB** of
pending frames per subscriber (26,214 `BookUpdate`s or 18,724 `Trade`s) on top
of a **64 KiB** socket send buffer requested from the kernel. A subscriber that
falls further behind is disconnected; other subscribers and order entry are
unaffected. A disconnected subscriber reconnects and starts from the
then-current sequence.

### Heartbeats and the trailing gap

A gap in a numbered stream is usually noticed when a later frame arrives. The
hard case is the last frame of a burst being lost: no later event follows, so
nothing reveals that the stream stopped one short. Decision C57 closes that hole
with an idle heartbeat (issue #151, merged as PR #165).

When the gateway's market-data stream has had no event for
`GatewayOptions::md_heartbeat_idle` (100 ms by default), the network thread
emits a `Heartbeat` to every subscriber whose header `seq` is the last sequence
number published. A heartbeat never consumes a sequence number, never enters the
routing state and is emitted at most once per idle interval. Its nonce is 0: a
gateway heartbeat is a stream marker, not a client round trip (the echo of a
client heartbeat still carries the client's nonce). An idle gateway that has
never published an event stays silent: there is no number to repeat, and a
heartbeat with `seq` 0 would tell a subscriber nothing.

The publisher forwards heartbeats over UDP like any other frame but never stores
them in the retransmit ring. A heartbeat repeats the number of a real event, so
storing it would either duplicate that event or — once the event had aged out —
move the ring backwards and discard recoverable history. `PublisherStats`
counts them separately in `heartbeats_forwarded`.

The feed handler treats a heartbeat whose `seq` is above the last applied
sequence as a gap and recovers the missing range; a heartbeat at or below the
last applied number is a no-op and never changes the book. Recovery is bounded
by the highest number the stream has announced (from applied frames, buffered
frames or heartbeats), not by how many frames happen to be buffered, so the
trailing case — a gap with nothing after it — is recovered. A `SequenceReset`
also settles every number below its `next_seq`, which closes a trailing gap that
the store could not answer.

### The publisher

`lob_publisher` is one single-threaded poll loop. It connects to the gateway's
market-data stream, frames it with a `StreamFramer`, and for every frame:

1. appends it to a preallocated retransmit store;
2. appends it to the current UDP datagram until the next frame would push the
   datagram past **1200 bytes**, then sends that datagram to every
   `--udp-dest host:port`.

Frames are forwarded unchanged and keep their market-data sequence in the
header. Destinations are resolved once at start-up, so a name must exist before
the publisher starts; `--mcast-group addr:port` adds one send-only multicast
destination, while unicast is the tested path. The publisher reconnects to the
gateway with a backoff that starts at 100 ms and doubles to a 5 s ceiling.

The retransmit store keeps the last **65,536** messages by default and accepts
`--retransmit` up to **1,048,576**; older messages are dropped as newer ones
arrive. Sequence numbers are the market-data numbers in the frame headers; a
duplicate is ignored, and a number moving backwards to one the store no longer
holds means the stream restarted, so the old contents are dropped first.

The recovery service listens on TCP **9002** and serves up to 64 concurrent
recovery clients from the same poll loop, so a slow recovery never stalls live
publishing. A `RecoveryRequest(from, to)` is answered with the frames in that
range that are still held, clamped to the newest published sequence. A request
whose `from` is older than the oldest held frame — or any request while the
store is empty — is answered with `SequenceReset{oldest held}` (`1` when nothing
is held). A request that starts after the newest frame gets no response; one
that asks for more than the store's capacity from a held sequence is refused and
its client disconnected.

### The feed handler

`lob_md_listen` owns one `FeedHandler`: a UDP socket (port 9100 by default), a
local price-level book and a TCP client to the recovery service. It expects the
publisher's sequence to advance by one. A frame that arrives ahead of the
expected number is buffered (up to 4,096 messages) and applied when the hole
closes, either from late datagrams or from recovery; a `SequenceReset` moves the
expectation to the named sequence. The handler asks for at most 1,024 messages
per request and retries a failed exchange after a 1 s timeout.

The counters distinguish what a subscriber can see: frames read from live
datagrams (duplicates included), duplicates, detected gaps, recovered gaps,
resets, recovery requests and failures, recovered messages, dropped and
malformed frames, trades, and heartbeats. A gap filled by late datagrams is
detected but not counted as recovered, because the recovery service was not
involved. `lob_md_listen` prints the top of its local book and the counters
every second (`--udp-port`, `--bind`, `--recovery`, `--top`, `--seconds`).

### The load generator

`lob_loadgen` opens `--sessions` order-entry connections and sends a seeded
order flow in a **closed loop**: each session has at most one request in flight,
and request `n+1` is sent only after request `n` has its first response. Each
request is stamped immediately before `send()` and the latency ends when the
first response frame that references the request's order id is decoded, after
`select()` reports the socket readable. That is the wire-to-wire definition used
throughout the project: it includes the client's syscall, both processes, the
kernel loopback path and the decode, on the same host.

The flow is roughly 70 percent new orders, 20 percent cancels and 10 percent
modifies, with a slowly drifting mid; client order ids always fit the 40 bits
the gateway reserves. `--rate` limits the aggregate rate, and a request still
unanswered after `--timeout-ms` ends the run. Latencies go into an HDR histogram
with nearest-rank percentiles (`p50`, `p90`, `p99`, `p99.9`, `max`, mean), and
samples above the histogram ceiling are clamped and counted rather than dropped.

The JSON report (`--report`) carries the run parameters, sent and answered
counts, `orders_without_response`, response counts by type, the latency
distribution, and — when `--md-udp-port` is set — the market-data counters from
a monitor thread running a `FeedHandler`. The exit status is non-zero when any
order was unanswered or the run failed. The report also embeds a `methodology`
object so a consumer does not have to infer the measurement:

> **Closed loop, one request in flight.** A slow response delays the next
> request instead of queueing behind it, so the percentiles describe an
> interactive client's steady state, not an open-loop stream's worst-case
> service time. This is the coordinated-omission trade-off: the harness does not
> schedule sends against a fixed arrival process, so it cannot report the
> latency a request would have seen if the system had stayed busy during a slow
> response. It is a closed-loop latency benchmark, and the report says so.

### The published measurement

`scripts/run_phase23_benchmarks.sh` is the only sanctioned source of Phase 3
latency numbers (decision C59). It waits for the 1-minute load average to fall to
`LOB_BENCH_MAX_LOAD`, builds the release preset, runs five repetitions of the
ring harness and five wire-to-wire runs at each session count, and renders the
report with `scripts/phase23_report.py`, which keeps the median run by p50 and
records the machine, toolchain and load that produced it. The figures below are
one such run on an idle machine; latency in nanoseconds, median run per session
count.

| Statistic | 1 session | 4 sessions |
| --- | --- | --- |
| p50 | 25,679 | 35,711 |
| p50 across runs | 25,263–26,127 ns | 34,303–37,311 ns |
| p90 | 45,823 | 63,231 |
| p99 | 211,583 | 101,951 |
| p99.9 | 215,935 | 137,343 |
| max | 2,887,679 | 3,186,687 |
| Requests/s (median run) | 30,172 | 94,578 |
| Orders per run | 200,000 | 200,000 |
| Unrecovered market-data gaps | 0 | 0 |
| Runs | 5 | 5 |

Apple M5 (Mac17,2, arm64, 32 GiB), macOS 27.0.1 (26A434), Apple clang 21.0.0
(clang-2100.3.34.2), `-O3 -DNDEBUG`, per-request `std::chrono::steady_clock`
timing, 5 runs of 200,000 orders at 1 session and 5 at 4 sessions, 2026-10-29
(UTC), commit `3acb776`; 1-minute load average 1.97 before the run and 3.21
after it. Every run in the median set had zero unanswered orders and zero
unrecovered gaps.

The median is dominated by the plumbing rather than by matching: each request
crosses the kernel's loopback path twice, wakes the gateway's network thread,
runs through two rings and wakes the load generator's poll loop, while the
engine's own share is a few hundred nanoseconds. Four sessions therefore cost
median latency — 35.7 µs against 25.7 µs, because the gateway's one network
thread and one engine thread serve four clients in turn — but improve p99 from
212 µs to 102 µs and throughput from 30,172 to 94,578 requests/s, because with
several requests in flight neither process is idle long enough to be descheduled
between a request and its response; the 2.9 ms max is one scheduling stall seen
end to end. The closed loop is the important caveat:
one request in flight per session means a slow response delays the next request
instead of queueing behind it, so these are an interactive client's steady-state
numbers and not a service time under load.

### The wiring test

The four executables are also tested together, not only as libraries. The
end-to-end wiring test (decision C58; issue #153, merged as PR #166) starts the
real `lob_gateway`, `lob_publisher`, `lob_md_listen` and `lob_loadgen` on
loopback, with no in-process fakes:

- `scripts/e2e_local.sh GATEWAY PUBLISHER MDLISTEN LOADGEN [orders] [sessions]`
  picks five distinct loopback ports from one random block in the unprivileged
  range and retries the whole run with a fresh block if a bind reports "address
  already in use";
- it starts the gateway, waits for both TCP listeners, starts the listener first
  so its UDP socket is bound before any datagram can be sent, starts the
  publisher, waits for its recovery listener and its "connected to gateway"
  line, then runs the load generator (2 sessions and 50,000 orders by default);
- readiness is polled every 20 ms with a 20 s timeout, never by sleeping a fixed
  interval; the script stops exactly the PIDs it started (TERM, then KILL after
  2 s, on every exit path) and never looks processes up by name;
- it fails unless the report shows `orders_without_response: 0`, no timeout, at
  least one market-data frame and zero unrecovered gaps, and it also fails if
  any of the three long-running processes died during the run.

`CMakeLists.txt` registers the script as `E2E.LocalWiring` (label `e2e`,
timeout 120 s) when all four targets exist, with `$<TARGET_FILE:…>` paths so it
works from any build directory and preset. It therefore runs in `ctest` on
macOS under every preset and in the Linux CI job and the Docker image, which
build the same four executables. The test's only dependency is bash and the
binaries — no Python, no `pgrep`, no fixed ready wait — so it works in the
minimal Ubuntu image as well.

## Key decisions and reasoning

- **Two threads and three rings, not a thread per connection.** The engine must
  never block on a socket, and the network side must never touch the book. A
  fixed ring pair makes the handoff allocation-free and keeps the book's state
  single-threaded, which is what makes the engine's no-allocation and
  deterministic-event guarantees true in a server.
- **Commands are decoded on the network thread and applied on the engine
  thread.** Decoding a frame needs no book state, so it stays off the engine.
  The ring carries a fixed-size value, not a pointer into a socket buffer.
- **Responses and market data both go back through rings.** The engine writes
  encoded frames into a stack buffer and pushes them; the network thread owns
  the sockets, sequence stamping and subscriber fan-out. That keeps the engine
  free of partial-write and backpressure logic.
- **One connection is one session, with no logon.** v2.0 has no authentication
  requirement and no session-resume message; a session id derived from the
  connection avoids a client-controlled field and keeps the messages small. The
  cost — a session that cannot migrate — is documented.
- **Packed order ids.** `(session_id << 40) | client_order_id` lets the engine
  keep caller-supplied ids while the gateway routes a response to a session
  without a second map. Forty bits is enough for a client's lifetime of ids and
  is rejected explicitly rather than truncated.
- **A stream sequence, not a per-connection one, on market data.** Every
  subscriber must be able to detect the same gap in the same place, and the
  publisher must be able to answer recovery by number. A per-connection counter
  would make both impossible.
- **Idle heartbeats that repeat the last sequence.** A heartbeat is a marker,
  not an event: consuming a number would make the stream's numbering depend on
  wall-clock idleness and would put a hole in the retransmit store that recovery
  could never fill. Repeating the last number makes a lost tail visible without
  changing the stream.
- **Heartbeats are forwarded but not stored.** The publisher's store is for
  events a subscriber may have missed. A heartbeat is not an event; storing it
  would duplicate a sequence or, worse, move the ring backwards and throw away
  real history.
- **Recovery is bounded by the highest announced number.** Recovery used to be
  driven by buffered frames, which is empty in exactly the trailing-gap case.
  Tracking the highest number the stream announced makes the gap visible even
  when there is nothing after it.
- **Datagrams never split a frame; 1200 bytes.** A frame is at most 56 bytes, so
  packing whole frames up to 1200 bytes keeps every datagram inside the smallest
  MTU any hop has to carry and keeps the receiver's framing trivial.
- **A closed loop for latency, with the trade-off stated.** An open-loop
  generator would need coordinated-omission correction or it would report a
  service time for requests that were never sent on time. The closed loop
  measures what one interactive client sees, reports that in the JSON, and does
  not claim to be a saturation benchmark.
- **The wiring test uses the real binaries.** An in-process test proves the
  components; it cannot catch a port default, a readiness race, a shutdown that
  leaves a process alive or a report field that the script reads under a
  different name. The script is deliberately dependency-free so the same test
  runs on macOS, Linux and inside the minimal image.

## Interfaces for future epics

- **`lob_net` is the shared protocol.** The gateway, publisher, feed handler and
  load generator link it; no service reimplements a byte layout. A new message
  id is the extension point (Epic 08).
- **The gateway is a library plus a `main`.** `lob::net::Gateway` exposes the
  chosen ports, the published and forwarded sequence numbers, subscriber and
  session counts, and a graceful `stop()`. Tests and future front ends drive the
  class directly.
- **The event stream is the contract for market data.** Anything that speaks
  `BookUpdate`/`Trade`/`Heartbeat`/`SequenceReset`/`RecoveryRequest` over the
  §14 framing can subscribe or recover; the publisher's UDP hop is one transport
  for it, not the definition.
- **`RetransmitStore` is independent of sockets.** Its `plan(from, to)` returns
  `Replay`, `Reset` or `Nothing`, which makes recovery policy testable without a
  network and reusable by a future snapshot or persistence layer.
- **`FeedHandler` is independent of the listening loop.** `process_datagram` and
  `poll` are both public, so a future client can feed it from another transport
  or drive it synchronously in a test.
- **The report schema is versioned.** `schema_version: 1` and the embedded
  `methodology` block are what scripts and the README quote; a change to the
  measurement has to bump the schema rather than silently change a field.
- **The wiring test is a ctest target.** Any future service that joins the
  topology can be added to `scripts/e2e_local.sh` and inherits the port
  selection, readiness and cleanup rules.

## Deviations

- **No snapshot on subscribe.** A late subscriber starts at the current
  sequence and builds from later updates. §15.3 documents this; a snapshot is a
  v2.1 concern, and the heartbeats only make a lost tail visible, not a
  subscriber's entire starting state.
- **The load generator's market-data monitor binds loopback.** The monitor's
  default is `127.0.0.1`, so a containerized load generator cannot receive the
  publisher's bridge traffic without sharing the publisher's network namespace.
  The Compose demo uses that namespace and a loopback destination for it; a
  bind-address flag is a follow-up, not part of this epic.
- **No cancel-on-disconnect.** A session's resting orders stay in the book after
  the connection closes. This is deliberate for v2.0 — there is no
  session-cancel message — and it means a client that disconnects can leave
  liquidity behind.
- **The recovery service has a fixed client limit.** It serves up to 64
  concurrent recovery clients from the poll loop. A larger subscriber fleet
  would need a different fan-out, and a slow recovery is bounded by the same
  loop that publishes.
- **Heartbeats start only after the first event.** An idle gateway with no
  published sequence is silent; a `seq` of 0 would be a marker with no meaning.
  A subscriber that connects to an idle book therefore hears nothing until the
  first event, which is also when it has something to build on.
- **The wiring test asserts at least one market-data frame.** The issue's
  minimum is zero unanswered and zero unrecovered gaps; a publisher that never
  sent anything would satisfy both, so the test also requires evidence that the
  market-data path carried traffic.
- **The end-to-end test runs under every preset.** It starts real processes and
  waits on their sockets, so it is slower than a unit test (1.8 s to 58.7 s
  observed) and is not the place to look for a deterministic microsecond
  number; the 120 s timeout is the bound.

## Known limitations

- **No authentication, logon or session resume.** The session begins at connect
  and ends at close; a reconnecting client gets a new session and cannot reclaim
  its old one.
- **No snapshot message.** A subscriber that connects to a long-running stream
  starts from the current sequence and has no way to obtain the book as of that
  point.
- **No checksum on the market-data path.** A corrupted UDP payload is detected
  only if it fails a length, reserved or enum check; sequence numbers and
  recovery close gaps but do not verify a plausible-looking wrong value.
- **The publisher resolves destinations once.** A DNS change or a listener that
  starts after the publisher is not picked up until the publisher restarts; the
  Compose demo therefore uses static addresses and `depends_on` ordering.
- **The retransmit store is memory-bound.** 65,536 frames by default (up to
  1,048,576) is the recovery window; a subscriber that falls further behind gets
  a `SequenceReset` and must rebuild from live traffic.
- **The load generator is closed-loop.** It measures an interactive client and
  explicitly does not model coordinated omission; it is not a saturation or
  open-loop benchmark.
- **Latency figures are one machine's.** The numbers in *The published
  measurement* describe this Mac, these flags and one commit; they are not a
  cross-machine comparison, and container runs are functional results that are
  never published as latency.
- **The wiring test needs four binaries and a loopback stack.** It is skipped
  when the targets are not built (the Linux benchmark-off build still builds the
  services), and it cannot run on a host without the ability to bind loopback
  ports.
- **The feed handler keeps one book.** It has no multi-instrument or
  multi-symbol routing; a second instrument would need a second process or a
  symbol field that the protocol does not have.
