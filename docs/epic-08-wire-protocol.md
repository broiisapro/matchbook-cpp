# Epic 08 — the wire protocol

## Goal

Phase 3 splits one matching engine into several processes: a gateway, a
market-data publisher, a feed handler and a load generator. They are written
independently and run on macOS and Linux arm64, so they need one byte-level
contract that does not depend on the compiler's struct layout, the host's byte
order or the size of the chunks a socket happens to deliver.

Epic 08 provides that contract as the `lob_net` static library: a fixed-size
little-endian message table, an encoder and decoder that touch one field at a
time, and a stream framer that turns arbitrary partial reads into complete
frames without allocating. It covers order entry, order-entry responses,
market data and the recovery handshake — every message the Phase 3 processes
exchange. The authoritative byte layouts live in `docs/architecture.md` §14;
this document explains how they are implemented and why.

The protocol is deliberately small. Twelve messages, a 16-byte header, a
40-byte maximum body and no optional fields: a frame is always 56 bytes or
fewer, which lets the framer hold an entire frame inline and lets a decoder
validate a header before the body has arrived.

## How it works

### The common header

Every frame starts with 16 bytes:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | type | u16 | message id (1–12), appended only |
| 2 | length | u16 | body size in bytes, header excluded |
| 4 | seq | u64 | sender sequence number |
| 12 | reserved | u32 | must be zero |

`length` counts body bytes only, so a frame occupies `16 + length` bytes; this
keeps the header out of the size arithmetic and makes the framer's fixed bound
(`16 + 40`) obvious. Reserved bytes are explicit rather than implicit padding:
an encoder writes zero and a decoder rejects a non-zero value. That is what
makes the layout independent of the compiler, and it is tested rather than
assumed.

`seq` has a different owner on different streams. On order-entry connections it
is the gateway's per-connection frame counter, starting at 1 and increasing by
one per frame written. On the market-data stream it is the single gap-free
stream sequence described in §15.3, and the recovery connection reuses those
numbers: a replayed frame keeps the sequence it was published with, and a
`SequenceReset` carries the sequence the subscriber should continue from. The
protocol carries the number; the process documents what it numbers.

### The message table

Twelve fixed-size messages, in the order their ids were assigned:

| Id | Message | Direction | Body |
|---|---|---|---|
| 1 | NewOrder | client → gateway | 32 |
| 2 | Cancel | client → gateway | 16 |
| 3 | Modify | client → gateway | 24 |
| 4 | Heartbeat | client → gateway | 8 |
| 5 | Ack | gateway → client | 16 |
| 6 | Reject | gateway → client | 16 |
| 7 | Fill | gateway → client | 40 |
| 8 | Cancelled | gateway → client | 16 |
| 9 | BookUpdate | publisher → subscriber | 24 |
| 10 | Trade | publisher → subscriber | 40 |
| 11 | SequenceReset | publisher → subscriber | 16 |
| 12 | RecoveryRequest | subscriber → recovery | 16 |

Per-field offsets are in `docs/architecture.md` §14.2. The semantic conventions
that matter when reading a frame:

- Prices are `int64_t` ticks and quantities `int64_t` lots, the same types the
  engine uses; the wire never carries floating point.
- `NewOrder.price` is ignored for a market order and sent as 0. `Modify` carries
  the new *remaining* quantity, not a delta. `BookUpdate.qty` is the new total
  at the level and 0 means the level is gone. `Cancelled.qty` is the quantity
  removed.
- `Fill.price` is the maker's resting price, and a fill is sent to both sides of
  the trade with the maker and taker ids in fixed positions.
- `Heartbeat.nonce` is chosen by the client and echoed, so a client can measure
  round-trip time without another message.
- `RecoveryRequest` asks for `[from_seq, to_seq]` inclusive;
  `SequenceReset.next_seq` is the first sequence the subscriber should expect
  next.
- Enum-valued fields use the values from `lob/types.hpp`: side 0 buy / 1 sell,
  order type 0 limit, 1 market, 2 IOC, 3 FOK, 4 post-only, and reject reasons
  from 1 through the range's maximum (the `WouldCross` fix below raises that
  maximum). Decoders reject values outside those ranges with `InvalidField`.

Message ids are appended only. Removing or renumbering one would silently change
the meaning of bytes already on the wire.

### The byte-by-byte codec

`net/protocol/codec.{hpp,cpp}` has one `encode` and one `decode` per message.
Encoding writes each field with explicit shifts (`store_u16`, `store_u32`,
`store_u64`), decoding assembles each field the same way (`load_u16`, …). No
struct is ever copied to or from a buffer as a block, so padding, alignment and
host byte order cannot leak into the format. The structs in `messages.hpp` carry
their field offsets and `static_assert`s tie the offset arithmetic to the body
size, so a field added without updating the codec fails the build.

Encoding into a recycled buffer is safe: `begin_frame()` writes the header,
zeroes the entire body, and then the message's own fields are stored. A reserved
byte can therefore never contain a previous frame's data.

Decoding validates in a fixed order and stops at the first failure:

1. `Truncated` — fewer bytes than the frame needs.
2. `BadReserved` — the header's reserved field is not zero.
3. `Oversize` — the declared body is larger than the largest message.
4. `UnknownType` — the type id is not in the table.
5. `BadLength` — the length does not match that type's body size.
6. `BadReserved` — a reserved byte in the body is not zero.
7. `InvalidField` — an enum-valued field is outside its range.
8. `WrongType` — the header names a different message than the caller asked to
   decode.

Only after every check passes are the output message and sequence number
written. A decoder can therefore be handed a frame it did not expect and leave
the caller's state untouched. `decode_header()` works on the 16 header bytes
alone, which is what lets the framer report a bad header without waiting for a
body that may never come.

The decoder accepts a buffer longer than the message and reads only the first
`kWireSize` bytes, so a caller can walk a buffer frame by frame.

### The framer

`net/protocol/framer.{hpp,cpp}` provides `StreamFramer`, one instance per
connection and not thread-safe. It holds at most one frame in an inline
`std::array<std::byte, kMaxWireSize>` and never allocates.

`push(chunk, sink)` works in a loop:

1. Copy bytes until 16 header bytes are buffered. If the chunk ends first, the
   frame stays partial and `push` returns `Ok`.
2. As soon as the header is complete, validate it **immediately**, before any of
   the body has arrived. A peer that sends a bad header is rejected without
   waiting for a body that may never be sent.
3. Copy the rest of the body into the buffer. If it is not all there yet, keep
   what arrived and return.
4. Hand the complete frame to the `FrameSink::on_frame(std::span<const std::byte>)`
   callback. The span points into the framer's buffer and is valid only for the
   duration of the call; a sink that keeps bytes must copy them.

`push()` returns a `PushResult { result, consumed }`. `consumed` is the number of
input bytes the framer took, which is all of them on `Ok` and the bytes up to a
stop or an error otherwise. A sink that returns `false` stops the call with
`Stopped`; the caller resubmits the unconsumed bytes when it is ready, so a
bounded queue downstream can apply backpressure without losing or duplicating a
byte. The sink is a virtual interface, following the project's choice of a
virtual `Listener` over templates (decision C5).

A protocol error clears the buffer and latches: later pushes return `Error`
without consuming input until `reset()`. There is no attempt to resynchronise on
the byte stream — a fixed-size binary protocol has no self-synchronising marker
— so a connection that produced a bad header is closed and reopened by its
owner. `buffered()` and `frames_delivered()` expose the partial size and the
count for tests and diagnostics.

### Tests

The protocol suite is `tests/test_protocol_messages.cpp` (20 tests) and
`tests/test_protocol_framer.cpp` (12 tests) — 32 tests, the count the protocol
check reported when the framer and codec merged.

The framing tests build a stream of 60 mixed messages — five of each type, every
field varied — 2,280 bytes in total. `SplitsAtEveryByteBoundary` pushes that
stream in two chunks for **every** split point, all 2,281 of them, and requires
the same frame sequence every time. `HandlesOneByteAtATime` pushes it one byte
per call, `HandlesRandomChunkSizes` uses five seeds with chunks of 1–23 bytes,
and `DeliversOneFramePerWholeFrameChunk` uses one frame per call. Every variant
must reproduce the identical frames with nothing left buffered. Further tests
cover a truncated final frame, a partial header, a header rejected before its
body arrives, error latching until `reset()`, stop-and-resume with exact
consumed counts, empty chunks, a sink that decodes every delivered frame, and
random garbage streams.

The codec tests round-trip every message and extreme field values (maximum
`u64`, minimum and maximum `i64`, zero), check two hand-built golden layouts
(`NewOrder` and `Reject`) byte for byte, and decode them back. Negative tests
cover truncation at every length, the wrong type, an unknown type id, a length
that does not match the type, an oversize length, non-zero header and body
reserved bytes, out-of-range enums, and short encode buffers. Robustness is
tested by flipping every byte of a valid frame to `0x00` and to `0xFF`, decoding
2,000 random buffers and pushing 200 random streams through the framer; under
the `asan` preset these also prove that no path reads outside the supplied
bytes.

### The `WouldCross` fix

The protocol landed with the reject reasons capped at 7
(`kMaxWireRejectReason = 7`), which was correct when `WouldCross` had no engine
path: `OrderType` and `RejectReason` were reserved ahead of implementation, and
the wire range covered the reasons the engines could emit at the time. Post-only
then shipped with `RejectReason::WouldCross = 8`. The encoder writes the value
happily, but every decoder rejects the frame as `InvalidField`, so a gateway
client that submits a crossing post-only order cannot read the rejection it
receives.

Issue #150 (merged as PR #161) fixed the range at the wire layer: the maximum
is derived from the last `RejectReason` value and a `static_assert` pins the
constant to the enum, so a future appended reason cannot silently fall outside
the range again. The change added a round trip of every `RejectReason` value
through the codec, a gateway test that submits a crossing post-only order and
receives `Reject(WouldCross)`, and a companion test that an unfillable FOK
receives `Cancelled` with no `Fill`. It also aligns the §14 reject-reason line
with the constant.

## Key decisions and reasoning

- **Fixed-size frames with an explicit length.** Variable-length encoding would
  need a length prefix anyway and would make the framer's buffer bound dynamic.
  A 40-byte maximum body means one 56-byte frame fits inline, the framer never
  allocates, and a decoder's work is bounded by a constant.
- **`length` counts body bytes, not the whole frame.** The header is then
  self-describing without including itself in its own size arithmetic, and the
  framer's total is `kHeaderSize + length`.
- **Field-by-field little-endian, never `memcpy`.** The services run on arm64
  macOS and Linux, but the format should not depend on that, and the compiler is
  free to pad. Writing the shifts once also makes the golden byte tests
  meaningful: they compare against the documented layout, not against a struct's
  image.
- **Reserved fields are explicit and must be zero.** Relying on implicit
  padding would make the format depend on the compiler; accepting junk in a
  reserved field would remove the ability to add a field there later. Encoders
  zero the whole body before writing, so a recycled buffer cannot leak.
- **Validate the header before the body arrives.** A peer that sends a bogus
  type id or length is detected after 16 bytes instead of after a frame that may
  never complete. It also keeps the framer's error handling simple: header
  errors are framer errors, body errors belong to the decoder.
- **A virtual `FrameSink`, not a template.** Matching the listener decision
  (C5), the framer takes an interface so the framer itself can stay in a `.cpp`
  with a stable ABI, and the per-frame virtual call is negligible next to a
  socket read. The `consumed` contract gives the caller backpressure without the
  framer owning a queue.
- **Errors latch instead of resynchronising.** With fixed-size frames and no
  magic bytes there is no safe place to resume; guessing would turn a protocol
  error into a stream of misinterpreted frames. The connection is closed instead.
- **Wire enum values are pinned to `lob/types.hpp`.** `static_assert`s for side
  and order type mean a renumbering breaks the build rather than the protocol.
  FOK/post-only were accepted at the wire level before the engines implemented
  them (decision C47), so the engine work needed no protocol change.
- **Reject reasons are opaque to the wire layer.** The book owns their meaning;
  the codec only checks the range. That is why the range lives in
  `messages.hpp` (and not in the frozen `types.hpp`) and why the `WouldCross`
  fix is a constant plus a `static_assert`, not a semantics change.
- **No version field or handshake.** Both ends are built from the same
  repository and deployed together; a version byte would be unused state in
  every frame. Backward compatibility is handled by appending message ids and
  enum values, not by negotiating.

## Interfaces for future epics

- **`lob_net` is the only protocol surface.** `messages.hpp` holds the table,
  `codec.hpp` the encode/decode functions and `framer.hpp` the stream
  interface. The gateway, publisher, feed handler and load generator all link
  the same library; none of them reimplement a byte layout.
- **Adding a message.** Append a `MessageType` id and a `body_size()` case, add
  a struct with offsets and `static_assert`s, add `encode`/`decode` overloads,
  and extend the tests. The body must stay within `kMaxBodySize` (40 bytes);
  anything larger needs the framer's inline buffer bound changed too.
- **Adding a field to an existing message** is a breaking change, because every
  body is fixed-size and the length must match exactly. The safe path is a new
  message id; the alternative would be a version field that does not exist yet.
- **The framer is reusable beyond TCP.** It takes arbitrary chunks and works
  for a UDP payload that happens to contain several messages, or for a
  memory-mapped byte stream; the gateway's per-connection framer is just one
  consumer.
- **`decode_header()` is public.** A process that only needs to peek at a frame
  — for logging, for a router, or to size a read — can validate and inspect the
  header without a typed decoder.
- **The wire order-type values are shared with the flow-file format.**
  `OrderType` bytes mean the same thing in both, so a future change has to keep
  the two in step. Appending is safe in both.
- **Market data and recovery reuse the same messages.** `BookUpdate`, `Trade`,
  `SequenceReset` and `RecoveryRequest` are ordinary frames; a future snapshot
  message would be appended to the same table, not a separate format.

## Deviations

- **No protocol version or capability negotiation.** The services are built and
  deployed together; appending ids and enum values is the compatibility story.
  A mixed-version deployment is not supported, and the framer's error latching
  makes the failure loud rather than silent.
- **No session id in any message.** One accepted connection is one session; the
  gateway maps the connection to a session id internally. That keeps the order
  entry messages smaller and avoids a client-controlled identity field, at the
  cost of a session that cannot migrate between connections.
- **The framer does not interpret message bodies.** It validates the header and
  hands the frame on, so a body-level error (a reserved byte, an out-of-range
  enum) is reported by the typed decoder, not by `push()`. Its error enum has
  fewer values than `DecodeStatus` for that reason; the body statuses that can
  never come out of `decode_header()` are mapped to `BadLength` defensively.
- **A decoder accepts a longer buffer.** `decode()` reads only the first
  `kWireSize` bytes, so passing a buffer with trailing frames is not an error.
  Callers that need exact framing use the framer.
- **The reject-reason range was too narrow for one wave.** It shipped as 1–7
  while `WouldCross` is 8; the wire layer had no test that every `RejectReason`
  value round-trips, which is exactly the gap that allowed it. Issue #150
  (merged as PR #161) fixed the range and added that test.
- **Order types 3 and 4 were accepted before the engines implemented them.**
  The decoder has always accepted the full 0–4 range, so the protocol never had
  to change when Epic 06 landed. A decoder cannot tell whether the peer
  implements the behaviour; that is the engine's answer, expressed as
  `UnsupportedOrderType` or as normal events.

## Known limitations

- **No snapshot message.** A late market-data subscriber starts at the current
  sequence and must build its book from later updates. §15.3 documents this;
  the protocol has no `Snapshot` frame to fix it.
- **No authentication, logon or session resume.** The protocol starts at
  `NewOrder`; identity is the TCP connection. A reconnecting client gets a new
  session and its old resting orders stay in the book.
- **No checksum or integrity field.** A corrupted UDP datagram is detected only
  if it fails a length, reserved or enum check. The publisher's sequence numbers
  and the feed handler's recovery close gaps, but they do not detect a payload
  that decodes to a plausible wrong value.
- **No compression or batching in the protocol.** The publisher groups messages
  into UDP datagrams to stay under the path MTU; the wire format itself is one
  frame at a time.
- **One frame size ceiling.** `kMaxBodySize` is 40 bytes and the framer's buffer
  is sized from it; a future message with a larger body needs the constant, the
  buffer and the documented bound changed together.
- **No cross-version negotiation.** A newer peer that appends a message id is
  understood as `UnknownType` by an older peer, which latches an error and
  closes the connection. There is no "ignore unknown messages" mode.
- **The framer is single-threaded.** One instance per connection, no internal
  locking; a process that reads one connection from several threads must
  serialise access itself.
- **The protocol says nothing about latency.** This epic defines bytes and
  framing only; wire-to-wire and ring numbers are measured by the Phase 3
  harnesses and published separately.
