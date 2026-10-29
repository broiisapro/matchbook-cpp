# Epic 06 — fill-or-kill and post-only orders

## Goal

The Phase 2 order types are the ones a real trading client asks for first. A
**fill-or-kill** (FOK) order says "fill all of me now or do nothing"; a
**post-only** order says "add liquidity, never take it". Both are expressed by
the same `submit` call the engine already had — only the order type changes —
and both must produce exactly the same event sequence in the fast engine and in
the reference engine, or the differential test loses its meaning.

Epic 06 turns the contract that was frozen before implementation into behaviour:
the two types are implemented independently in both engines, the generators and
the flow-file format carry them, the Python bindings expose them, and a
differential test runs them against both engines at volume. The public API does
not grow: no new method, no new listener callback, no new configuration field.

The rollout deliberately did not start with the engines. `include/lob/types.hpp`
is frozen after Epic 0, and changing an enum value after it has been written to
flow files and log lines is not possible without renumbering. The contract and
the enum values therefore landed first, with both engines rejecting the new
types identically, so the engines could then be implemented in parallel without
ever producing a differential mismatch that was really a sequencing artefact.

## How it works

### The interim contract

Issue #90 (merged as #100) appended the values and froze the contract:

- `OrderType` gains `FOK = 3` and `PostOnly = 4`; `RejectReason` gains
  `UnsupportedOrderType = 7` and `WouldCross = 8`. Existing values keep their
  numbers, because the order type is a flow-file byte and reject reasons appear
  in logs and on the wire.
- Both engines added a validation **step 0**: a `FOK` or `PostOnly` submit is
  rejected with `on_reject(id, UnsupportedOrderType)` before the quantity,
  price and duplicate-id checks. Nothing else is emitted and the book is not
  touched. Being identical in both engines, the interim state kept the
  differential test exact instead of turning every new type into a mismatch.
- `docs/architecture.md` §4.1.1 and §4.1.2 gained the final semantics as the
  contract to implement, and the optional-mix work could be scheduled against
  them.
- The operation generator gained `fok_weight` and `post_only_weight` at about 2
  percent each, so the reject path itself was exercised in every preset while
  the implementation was still pending. FOK and post-only submits have never
  entered the generator's modelled book: that model is a heuristic for aiming
  cancels and modifies at live ids, and treating a non-resting order as absent
  keeps it honest.

### The rollout order

Decision C53 fixed the sequence, because the differential test compares the two
engines operation by operation:

1. First the default op mix stopped emitting the Phase 2 types (#111, merged as
   #132), with an opt-in `all_types_mix()` and `lobsim difftest --types all`.
   From that point a default CI run could never compare a FOK-implementing
   engine against a FOK-rejecting one.
2. The fast engine (#113, merged as #146) and the reference engine (#114,
   merged as #133) then implemented §4.1.1 and §4.1.2 independently, in either
   merge order.
3. Flow files and `lobsim gen` learned the types (#115, merged as #141), the
   Python bindings exposed them (#117, merged as #134), and the all-types
   differential test plus the nightly runs landed last (#116, merged as #148).

The all-types mix adds about 5 percent FOK and 5 percent post-only submits on
top of the existing market/IOC shares, and prices half of the post-only orders
through the opposite best level. A run therefore reaches the FOK kill path, the
FOK full-fill path, the `WouldCross` reject and the post-only rest path rather
than only one of them.

### FOK: the kill check before the first trade

A FOK is a limit order that must fill completely and immediately, or not trade
at all.

1. Validation is the ordinary §4.1 sequence without the interim step 0: the
   quantity, price and duplicate-id checks keep their order and their reasons.
2. `on_accept(id)` is emitted before any trade, exactly as for every other
   accepted order.
3. The kill check runs before the first trade. The engine sums the quantity
   resting at every opposite level the limit price crosses — asks at or below
   the price for a buy, bids at or above it for a sell — and stops as soon as
   the submitted quantity is covered. The walk is read-only: it reads level
   totals and never links, unlinks or updates anything, so a failed check can
   leave no trace.
4. If the crossing quantity is short, the order is killed with
   `on_cancel(id, qty)` carrying the **full submitted quantity**, not a
   remainder. No `on_trade` and no `on_book_update` is emitted, and the book
   digest is unchanged.
5. If the crossing quantity is enough, matching proceeds exactly as for an IOC
   that completes: one `on_trade` plus one `on_book_update` per maker touched,
   in price-time priority, no `on_cancel`.
6. A FOK never rests. A fully filled FOK leaves nothing behind and a killed FOK
   is cancelled before it can reach the resting path, so neither can hit the
   `max_orders` / `max_levels` capacity rule.

Two consequences are worth stating because they are easy to get wrong. The kill
decision depends on the **limit price**, not on total opposite-side depth: a
buy FOK at 100 against asks at 99, 100 and 101 can only count the first two
levels. And the filled quantity is taken at the makers' resting prices, as for
any other taker, so a FOK can fill better than its limit and never worse.

In the fast engine the check is `OrderBook::can_fill_immediately()`, a
best-first walk over the opposite ladder; in the reference engine it is
`crossing_qty()`, the same walk over the naive `std::map`. Both are private
helpers; neither is on the public API.

### PostOnly: reject on arrival, otherwise rest

A post-only order may only add liquidity.

1. Validation is the ordinary §4.1 sequence without step 0, in the same order:
   quantity, then price, then duplicate id.
2. The crossing check runs after those checks and **before** `on_accept`: if the
   opposite side's best price crosses the limit — best ask at or below the
   price for a buy, best bid at or above it for a sell — the order is rejected
   with `on_reject(id, WouldCross)`. Equality counts as crossing. Nothing else
   is emitted, the book is unchanged and the id is not live, so it can be
   reused immediately.
3. Otherwise the order rests exactly like a limit order: `on_accept(id)`, then
   the ordinary resting path for the full quantity, including the capacity
   rule. If `max_orders` or `max_levels` forbids a new resting order, the
   engine emits `on_cancel(id, qty)` after the accept, because the order was
   accepted and then could not rest.
4. The check is against the book as it is when the submit is applied. A
   post-only order that rests can of course be traded against by a later
   incoming order; that is the point of resting it.

The fast engine checks `would_cross()` in `submit()` before calling `place()`;
the reference engine checks the opposite map's best entry in the same position.
Both are read-only.

### Modifying a resting post-only order

The modify contract (§4.3) is written in terms of the order's price and
quantity, not its type. A same-price reduction is an in-place `on_modify` and
keeps queue priority. Any other modify is cancel + new, and step 6 says the new
order is submitted as a **`Limit`** order. A post-only order whose price is
moved therefore becomes a limit order and may trade. This is a contract
consequence, not an accident, and both engines implement it the same way.

### Both engines, and the all-types differential

The two implementations share no matching code. Their behaviour is pinned by
hand-written event vectors on each side:

- `tests/test_order_book_types.cpp` (fast engine, 22 tests): FOK exact fill from
  one maker and at the resting price rather than its limit; a three-level
  sweep; queue priority within a level; sells against bids; one lot short
  (killed, digest unchanged); a kill bounded by the limit price rather than
  total depth; kills on an empty book on both sides; a FOK against a book with
  `max_orders = max_levels = 1` proving it neither rests nor hits capacity.
  Post-only resting on an empty and a non-crossing book; `WouldCross` at the
  equal best price and through the book on both sides; the rejected id not
  being live and being reusable; capacity cancels for both limits; the
  validation order for both types; and both modify shapes of a resting
  post-only order.
- `tests/test_reference_types.cpp` (reference engine, 15 tests): the same
  contract written against the naive maps, including a partially filled last
  maker, a sell sweeping bids in descending price order, and a FOK that must
  not count a level its limit does not cross.

`tests/test_order_types.cpp` no longer asserts either engine's behaviour; it
pins only what both engines share, the appended enum values and their
`to_string` names, because the two interim cross-engine tests could not survive
one engine implementing the types before the other.

The differential suite closes the loop.
`FastVsReference.RandomOpsMatchReferenceAllTypes` replays 1,000,000 operations
(100,000 under ASan/TSan) from the all-types mix on the narrow configuration
(prices 100–200, 128 orders, 32 levels, seed 20261028) and requires identical
event vectors after every operation and identical digests. Its basic-mix
companion keeps the same configuration, so a regression in the Phase 2 paths is
attributable. The seeded invariant test also emits FOK (in a wide price band, so
both kills and fills occur) and post-only (in a limit band, so both
`WouldCross` and resting occur) and checks the book invariants after every
operation.

The nightly workflow runs four combinations per seed and ladder — narrow/basic,
narrow/all, wide/basic, wide/all — for both the `release` and `release-tree`
presets, with 2,000,000 narrow operations and 200,000 wide operations per run
by default and five seeds. The all-types runs are roughly twice as expensive as
their basic counterparts, which the job's timeout accounts for.

### Flow files and the generators

The flow-file record is unchanged at 40 bytes; the `type` byte was always a
`u8` with reserved bytes beside it, so the format gained values `3` (FOK) and
`4` (post-only) without moving anything. `FlowReader` accepts both, rejects
bytes above 4 with the record index, and `read_csv` accepts the words `fok` and
`post_only` that `to_string(OrderType)` already produced. `write_csv` needed no
change.

`lobsim gen --types basic|all` defaults to `basic`, whose output stays
byte-identical to a pre-#115 build for the same seed — pinned by a test on an
XXH64 of the generated file. `all` takes FOK and post-only at 5 percent of
submits each from the same single type roll used for market/IOC, so basic mode
does not consume any extra randomness. `lobsim replay` passes the record type
straight to `submit()` and its summary reports the submit count per type
(`types: limit=N market=N ioc=N fok=N post_only=N`), which makes the pass-through
observable without depending on which engine is replaying.

`lobsim difftest --types basic|all` selects the mix, composable with every
`--mix` preset, and the generator's `all_types_mix()` composes the same way
(`all_types_mix(wide_mix())`). Both defaults remain `basic`, per C53.

### The Python bindings

`pylob` gained `OrderType.FOK`, `OrderType.PostOnly`,
`RejectReason.UnsupportedOrderType` and `RejectReason.WouldCross`, with the
integer values of the flow-file bytes and the `to_string` words as member-name
sources (`fok`, `post_only`, `would_cross`). The binding does not change
matching behaviour; it maps the enum and documents the validation order in
`submit`.

The binding's tests detect the engine's state with a **probe** rather than a
version flag: they submit the type into an empty book and look for an
`UnsupportedOrderType` reject. A killed FOK and a resting post-only never
produce that reject, so the probe is unambiguous for both types. The final
semantics assertions were skipped while the engine was interim and became
active once #113 merged, with no test edit.

## Key decisions and reasoning

- **Contract first, with an identical interim reject.** Implementing the two
  engines in parallel against an unfrozen contract would have produced
  disagreements that look like bugs in matching. Rejecting at validation step 0
  in both engines kept the differential exact and let the enum values, the flow
  format and the wire protocol be fixed before any engine change.
- **Append, never renumber.** `OrderType` is a flow-file byte and
  `RejectReason` travels on the wire and into logs. Appending `FOK = 3`,
  `PostOnly = 4`, `UnsupportedOrderType = 7` and `WouldCross = 8` means an old
  file or log line still parses, and the protocol needed no change when the
  types started working.
- **The kill check is read-only and runs before the first trade.** The
  alternative — match until the book runs out, then undo — cannot be done
  without emitting trades to a listener that has already seen them. Summing
  crossing level quantities first makes the kill atomic from the listener's
  point of view.
- **A killed FOK cancels the full submitted quantity.** §4.1.1 says the full
  quantity "never rested", and a listener that tracks an order's lifecycle needs
  the cancel to close the quantity it saw accepted. Reporting an unfillable
  remainder instead would be a different number for no benefit.
- **Post-only checks crossing after validation and before accept.** That is the
  only ordering consistent with "nothing else is emitted": a `WouldCross` order
  must not produce an accept, and a quantity, price or duplicate-id error must
  keep its own reason even when the order would also cross.
- **Equality crosses.** A buy post-only at the best ask is taking liquidity, not
  adding it, so `best_ask <= price` is the test on both engines.
- **A modified post-only becomes a limit order.** §4.3's cancel+new step names
  `Limit` explicitly. Special-casing the type would either introduce a second
  modify contract or silently keep post-only semantics the caller did not ask
  for; both are worse than following the written contract.
- **Keep the Phase 2 types out of the default mixes.** C53's sequencing is a
  test-design decision: CI must not report a mismatch that only exists because
  one engine merged first. `--types all` makes the coverage explicit and
  opt-in, and the nightly runs it on both ladders.
- **The generator does not model FOK or post-only orders.** The model is a
  heuristic for choosing live ids to cancel or modify, and a non-resting order
  is absent from the book by definition. Adding them to the model would add
  state that has no effect on the stream it is used to produce.
- **Probe, don't version-check, in Python.** The binding and the engines merge
  independently. A probe tests what the engine actually does, so the same test
  file is meaningful before and after the engine change and cannot go stale
  when a version constant moves.

## Interfaces for future epics

- **No new public API.** `submit`, `cancel`, `modify`, the `Listener` callbacks
  and `Config` are unchanged. FOK and post-only are order-type values, and the
  only new observable is the existing `on_reject` with `WouldCross`.
- **Flow-file type bytes 3 and 4 are now data.** The 40-byte record and version
  byte are unchanged; a new order type would append another type byte value and
  another `to_string` word, not a new record layout.
- **Wire values are already reserved.** `net/protocol` accepts order types 0–4,
  so the engine's later implementation needed no protocol change. The reject
  range is a separate constant in `messages.hpp` and must be extended when a
  reason is appended; Epic 08 covers that fix for `WouldCross`.
- **Generator coverage is a flag, not a new preset.** `all_types_mix()` composes
  with the default, wide and ladder-stress mixes, so any future mix can opt into
  FOK/post-only without duplicating the price logic.
- **Test pattern for state-dependent semantics.** Probe the engine, gate the
  assertions with a reason, and keep the unconditional assertions for what is
  already true. This is the same pattern the Python tests use and it fits any
  later feature that rolls out across engines at different times.
- **Per-type counts in replay.** The `types: …` summary line is the place to add
  a new submit type; existing `key: value` consumers keep working.

## Deviations

- **The flow-file version stays 1.** Adding enum values does not change the
  record layout, so bumping the version would have broken every existing reader
  for no layout reason. The cost is that a pre-#115 binary reading a flow that
  contains FOK or post-only records fails with `InvalidRecord` rather than a
  version message; that is the loud failure the reader already gives for an
  unknown type byte.
- **`RejectReason::UnsupportedOrderType` is retained.** Neither engine emits it
  any more, but it is enum value 7, already written by the interim builds and
  accepted by the wire decoder. Removing it would renumber `WouldCross` and
  break both.
- **The cross-engine typed tests were retired.** `tests/test_order_types.cpp`
  now pins only the enum values and names. Its interim assertions could not hold
  for both engines once one of them implemented the types, and per-engine
  coverage moved to the two typed test files. The all-types differential is the
  cross-engine check.
- **Types are limit-only.** `FOK` and `PostOnly` are `OrderType` values, so an
  order is one type, not a type plus a time-in-force: there is no market FOK, no
  IOC post-only and no post-only FOK. The contract defines both as limit orders
  with a price that must be inside the configured range.
- **Modifying a post-only order can trade.** Following §4.3 means a price move
  is submitted as a limit order and may cross. A caller that wants to preserve
  "never take" must cancel and resubmit as post-only and accept the
  `WouldCross` rejection instead.
- **The FOK availability walk is linear in the crossed depth.** It stops once
  the quantity is covered, so a kill on a thin book is cheap, while a large FOK
  that sweeps many levels pays for the levels it actually needs. No cached
  depth was added; that would be a matching-engine change, not an order-type
  one.

## Known limitations

- **No market FOK.** A FOK must carry a limit price, so a client that wants
  "fill all or kill regardless of price" has to submit at the far edge of the
  configured range (`max_price` for a buy, `min_price` for a sell). That works
  because every resting price is inside the range, but it is a convention
  rather than a message type, and it is easy to get wrong.
- **`CapacityExceeded` remains unused by the engines.** Capacity shortages are
  reported as accept + cancel for the unrestable quantity, for post-only just
  as for limit orders. The reject reason exists in the enum and on the wire but
  no engine path emits it.
- **A post-only rejection may not reach a remote client yet.** The wire
  protocol's reject-reason range was capped below `WouldCross` when the protocol
  landed (#102); Epic 08's `WouldCross` fix (#150) extends it. Until that fix is
  merged, a gateway client that submits a crossing post-only order gets a frame
  its decoder refuses rather than a `Reject(WouldCross)`.
- **No per-type counters in the engine.** Counts exist in `lobsim replay`'s
  summary and in the generator's tests, not as engine statistics; a service
  that wants them has to count listener events.
- **The generator's book model ignores the Phase 2 types.** Cancels and modifies
  may therefore target ids that a FOK never rested or a post-only order never
  became. They are still valid stream operations (typically unknown-id
  rejects), but the mix's targeting is less precise for those orders.
- **No dedicated randomized FOK/post-only test beyond the differential.** The
  typed unit tests are hand-written and the all-types differential is the
  volume test; there is no standalone property test that isolates the two types
  from the rest of the mix.
- **Python final-semantics tests depend on the probe.** If a future engine
  returns `UnsupportedOrderType` for some other reason, the probe would treat
  the engine as interim and skip the final assertions rather than fail.
