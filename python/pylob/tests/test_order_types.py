"""FOK and post-only orders through the bindings.

The Phase 2 rollout can land the bindings and the engine semantics in either
order (decision C53), so the tests probe what the engine under test implements
instead of assuming a state. The interim assertion (both types are rejected
with ``UnsupportedOrderType``) runs on an engine without the semantics, and
the final-semantics assertions (``docs/architecture.md`` sections 4.1.1 and
4.1.2) are skipped there with a clear reason. The same test file therefore
passes before and after the engine work merges.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from flow_builder import (
    BUY,
    FOK,
    LIMIT,
    POST_ONLY,
    SELL,
    SUBMIT,
    Record,
    write_csv_flow,
    write_flow,
)

import pylob


def make_config(**overrides: int) -> pylob.Config:
    settings: dict[str, int] = {
        "min_price": 1,
        "max_price": 1_000_000,
        "max_orders": 64,
        "max_levels": 64,
    }
    settings.update(overrides)
    return pylob.Config(**settings)


def make_book(listener: object | None = None, **overrides: int) -> pylob.OrderBook:
    return pylob.OrderBook(make_config(**overrides), listener)


def rejects(listener: pylob.RecordingListener) -> list[pylob.Reject]:
    return [event for event in listener.events if isinstance(event, pylob.Reject)]


def trades(listener: pylob.RecordingListener) -> list[pylob.Trade]:
    return [event for event in listener.events if isinstance(event, pylob.Trade)]


def implements(order_type: pylob.OrderType) -> bool:
    """Probes whether the engine has the Phase 2 semantics for one type.

    The probe submits the type into an empty book. An engine that still
    follows the interim contract rejects it with ``UnsupportedOrderType``;
    one that implements the type accepts it (and then cancels a killed FOK or
    rests a post-only order).
    """
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Buy, order_type, 50_000, 1)
    return not any(
        event.reason == pylob.RejectReason.UnsupportedOrderType for event in rejects(listener)
    )


FOK_IMPLEMENTED = implements(pylob.OrderType.FOK)
POST_ONLY_IMPLEMENTED = implements(pylob.OrderType.PostOnly)

requires_fok = pytest.mark.skipif(
    not FOK_IMPLEMENTED, reason="the engine still rejects FOK with UnsupportedOrderType"
)
requires_post_only = pytest.mark.skipif(
    not POST_ONLY_IMPLEMENTED,
    reason="the engine still rejects post-only with UnsupportedOrderType",
)


def test_enum_members_use_the_wire_values_and_python_names() -> None:
    order_types = pylob.OrderType.__members__
    assert list(order_types) == ["Limit", "Market", "IOC", "FOK", "PostOnly"]
    assert [member.value for member in order_types.values()] == [0, 1, 2, 3, 4]

    reasons = pylob.RejectReason.__members__
    assert list(reasons) == [
        "DuplicateId",
        "UnknownOrder",
        "InvalidQuantity",
        "InvalidPrice",
        "CapacityExceeded",
        "ModifyNoChange",
        "UnsupportedOrderType",
        "WouldCross",
    ]
    assert [member.value for member in reasons.values()] == list(range(1, 9))


def test_enums_convert_from_their_names_and_from_wire_values() -> None:
    order_types = pylob.OrderType.__members__
    assert order_types["FOK"] == pylob.OrderType.FOK
    assert order_types["PostOnly"] == pylob.OrderType.PostOnly
    assert pylob.OrderType(3) == pylob.OrderType.FOK
    assert pylob.OrderType(4) == pylob.OrderType.PostOnly

    reasons = pylob.RejectReason.__members__
    assert reasons["UnsupportedOrderType"] == pylob.RejectReason.UnsupportedOrderType
    assert reasons["WouldCross"] == pylob.RejectReason.WouldCross
    assert pylob.RejectReason(7) == pylob.RejectReason.UnsupportedOrderType
    assert pylob.RejectReason(8) == pylob.RejectReason.WouldCross


@pytest.mark.skipif(FOK_IMPLEMENTED, reason="FOK is implemented; the final semantics are tested")
def test_fok_rejection_precedes_every_other_validation() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 1)
    listener.clear()

    book.submit(1, pylob.Side.Buy, pylob.OrderType.FOK, 100, 0)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.FOK, 0, 1)
    book.submit(1, pylob.Side.Buy, pylob.OrderType.FOK, 100, 5)

    assert listener.events == [
        pylob.Reject(1, pylob.RejectReason.UnsupportedOrderType),
        pylob.Reject(2, pylob.RejectReason.UnsupportedOrderType),
        pylob.Reject(1, pylob.RejectReason.UnsupportedOrderType),
    ]
    assert book.level_qty(pylob.Side.Sell, 100) == 1
    assert book.best_ask() == 100


@pytest.mark.skipif(
    POST_ONLY_IMPLEMENTED, reason="post-only is implemented; the final semantics are tested"
)
def test_post_only_rejection_precedes_every_other_validation() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)

    book.submit(1, pylob.Side.Buy, pylob.OrderType.PostOnly, 100, 0)
    book.submit(2, pylob.Side.Sell, pylob.OrderType.PostOnly, 0, 1)
    book.submit(3, pylob.Side.Sell, pylob.OrderType.PostOnly, 100, 1)

    assert listener.events == [
        pylob.Reject(1, pylob.RejectReason.UnsupportedOrderType),
        pylob.Reject(2, pylob.RejectReason.UnsupportedOrderType),
        pylob.Reject(3, pylob.RejectReason.UnsupportedOrderType),
    ]
    assert book.order_count() == 0
    assert book.best_bid() is None
    assert book.best_ask() is None


@requires_fok
def test_fok_is_killed_when_the_crossing_quantity_is_short() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 5)
    book.submit(2, pylob.Side.Sell, pylob.OrderType.Limit, 101, 100)
    listener.clear()
    digest = book.digest()

    book.submit(3, pylob.Side.Buy, pylob.OrderType.FOK, 100, 6)

    assert listener.events == [
        pylob.Accept(3),
        pylob.Cancel(3, 6),
    ]
    assert book.digest() == digest
    assert book.level_qty(pylob.Side.Sell, 100) == 5


@requires_fok
def test_fok_fills_completely_in_price_time_priority() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 3)
    book.submit(2, pylob.Side.Sell, pylob.OrderType.Limit, 101, 4)
    listener.clear()

    book.submit(3, pylob.Side.Buy, pylob.OrderType.FOK, 101, 6)

    assert listener.events == [
        pylob.Accept(3),
        pylob.Trade(1, 3, 100, 3),
        pylob.BookUpdate(pylob.Side.Sell, 100, 0),
        pylob.Trade(2, 3, 101, 3),
        pylob.BookUpdate(pylob.Side.Sell, 101, 1),
    ]
    assert book.order_count() == 1  # only the partially filled maker is left
    assert book.level_qty(pylob.Side.Sell, 101) == 1
    assert book.best_bid() is None


@requires_fok
def test_fok_never_rests_and_keeps_the_validation_order() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 50)
    listener.clear()

    book.submit(1, pylob.Side.Buy, pylob.OrderType.FOK, 90, 1)  # duplicate id
    book.submit(2, pylob.Side.Buy, pylob.OrderType.FOK, 100, 0)
    book.submit(3, pylob.Side.Buy, pylob.OrderType.FOK, 0, 1)

    assert listener.events == [
        pylob.Reject(1, pylob.RejectReason.DuplicateId),
        pylob.Reject(2, pylob.RejectReason.InvalidQuantity),
        pylob.Reject(3, pylob.RejectReason.InvalidPrice),
    ]

    listener.clear()
    book.submit(4, pylob.Side.Buy, pylob.OrderType.FOK, 90, 1)

    assert listener.events == [pylob.Accept(4), pylob.Cancel(4, 1)]
    assert book.order_count() == 1


@requires_post_only
def test_post_only_is_rejected_when_it_would_cross() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 4)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 98, 4)
    listener.clear()
    digest = book.digest()

    book.submit(3, pylob.Side.Buy, pylob.OrderType.PostOnly, 100, 1)
    book.submit(4, pylob.Side.Buy, pylob.OrderType.PostOnly, 105, 1)
    book.submit(5, pylob.Side.Sell, pylob.OrderType.PostOnly, 98, 1)
    book.submit(6, pylob.Side.Sell, pylob.OrderType.PostOnly, 90, 1)

    assert listener.events == [
        pylob.Reject(3, pylob.RejectReason.WouldCross),
        pylob.Reject(4, pylob.RejectReason.WouldCross),
        pylob.Reject(5, pylob.RejectReason.WouldCross),
        pylob.Reject(6, pylob.RejectReason.WouldCross),
    ]
    assert book.digest() == digest
    assert book.order_count() == 2


@requires_post_only
def test_post_only_rests_away_from_the_touch_and_keeps_priority() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 4)
    listener.clear()

    book.submit(2, pylob.Side.Buy, pylob.OrderType.PostOnly, 99, 3)
    book.submit(3, pylob.Side.Buy, pylob.OrderType.PostOnly, 99, 2)

    assert listener.events == [
        pylob.Accept(2),
        pylob.BookUpdate(pylob.Side.Buy, 99, 3),
        pylob.Accept(3),
        pylob.BookUpdate(pylob.Side.Buy, 99, 5),
    ]
    assert book.best_bid() == 99
    assert book.best_ask() == 100
    assert book.level_qty(pylob.Side.Buy, 99) == 5

    listener.clear()
    book.submit(4, pylob.Side.Sell, pylob.OrderType.Limit, 99, 4)

    assert [(trade.maker_id, trade.qty) for trade in trades(listener)] == [(2, 3), (3, 1)]
    assert trades(listener)[0].price == 99


@requires_post_only
def test_post_only_that_cannot_rest_is_accepted_then_cancelled() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener, max_orders=1)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 1)
    listener.clear()

    book.submit(2, pylob.Side.Buy, pylob.OrderType.PostOnly, 99, 2)

    assert listener.events == [
        pylob.Accept(2),
        pylob.Cancel(2, 2),
    ]
    assert book.order_count() == 1


def flow_with_fok_and_post_only() -> list[Record]:
    """A flow that exercises both types: a rest, a kill, a cross and a fill."""
    return [
        (SUBMIT, SELL, LIMIT, 1, 100, 4, 1_000_000),
        (SUBMIT, BUY, POST_ONLY, 2, 99, 2, 2_000_000),  # rests at 99
        (SUBMIT, BUY, FOK, 3, 100, 5, 3_000_000),  # only 4 available: killed
        (SUBMIT, BUY, POST_ONLY, 4, 100, 1, 4_000_000),  # crosses: WouldCross
        (SUBMIT, BUY, FOK, 5, 100, 4, 5_000_000),  # fills the ask completely
    ]


def write_types_flow(tmp_path: Path, records: list[Record], form: str) -> Path:
    if form == "csv":
        return write_csv_flow(tmp_path / "types.csv", records)
    return write_flow(tmp_path / "types.bin", records)


@pytest.mark.parametrize("form", ["binary", "csv"])
def test_read_flow_exposes_the_new_types_and_replay_matches_direct_application(
    tmp_path: Path, form: str
) -> None:
    records = flow_with_fok_and_post_only()
    path = write_types_flow(tmp_path, records, form)

    try:
        loaded = pylob.read_flow(str(path))
    except RuntimeError as error:
        if "invalid type" in str(error):
            pytest.skip("the flow-file reader does not accept FOK/post-only records yet")
        raise

    submits = [record for record in records if record[0] == SUBMIT]
    assert [(record.type, record.id) for record in loaded] == [
        (pylob.OrderType(record[2]), record[3]) for record in submits
    ]

    summary = pylob.replay(str(path))
    assert summary["records"] == len(records)
    assert summary["submits"] == len(submits)

    listener = pylob.RecordingListener()
    book = make_book(listener)
    for _op, side, type_, id_, price, qty, _ts_ns in records:
        book.submit(id_, pylob.Side(side), pylob.OrderType(type_), price, qty)

    assert summary["accepts"] == len(
        [event for event in listener.events if isinstance(event, pylob.Accept)]
    )
    assert summary["rejects"] == len(rejects(listener))
    assert summary["trades"] == len(trades(listener))
    assert summary["traded_qty"] == sum(trade.qty for trade in trades(listener))
    assert summary["final_orders"] == book.order_count()
    assert summary["digest"] == book.digest()
