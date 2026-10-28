"""Tests for the pylob OrderBook, config and event binding."""

from __future__ import annotations

import pytest

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


def make_book(listener: object | None = None) -> pylob.OrderBook:
    return pylob.OrderBook(make_config(), listener)


def events_of(listener: pylob.RecordingListener, kind: type) -> list:
    return [event for event in listener.events if isinstance(event, kind)]


def test_trade_prints_at_the_resting_price() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 10_005, 4)
    listener.clear()

    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 10_010, 4)

    assert listener.events == [
        pylob.Accept(2),
        pylob.Trade(1, 2, 10_005, 4),
        pylob.BookUpdate(pylob.Side.Sell, 10_005, 0),
    ]
    assert book.best_ask() is None
    assert book.order_count() == 0


def test_partial_fill_keeps_queue_priority() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 5)
    book.submit(2, pylob.Side.Sell, pylob.OrderType.Limit, 100, 5)
    listener.clear()

    book.submit(3, pylob.Side.Buy, pylob.OrderType.Limit, 100, 3)

    assert listener.events == [
        pylob.Accept(3),
        pylob.Trade(1, 3, 100, 3),
        pylob.BookUpdate(pylob.Side.Sell, 100, 7),
    ]
    assert book.level_qty(pylob.Side.Sell, 100) == 7

    book.submit(4, pylob.Side.Buy, pylob.OrderType.Limit, 100, 4)

    trades = events_of(listener, pylob.Trade)
    assert trades[-2:] == [pylob.Trade(1, 4, 100, 2), pylob.Trade(2, 4, 100, 2)]
    assert book.level_qty(pylob.Side.Sell, 100) == 3


def test_modify_reduce_keeps_priority_and_price_change_moves_to_back() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Buy, pylob.OrderType.Limit, 100, 5)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 100, 5)

    book.modify(1, 100, 2)
    assert listener.events[-2:] == [
        pylob.Modify(1, 100, 2),
        pylob.BookUpdate(pylob.Side.Buy, 100, 7),
    ]

    listener.clear()
    book.submit(3, pylob.Side.Sell, pylob.OrderType.Limit, 100, 3)
    assert [(trade.maker_id, trade.qty) for trade in events_of(listener, pylob.Trade)] == [
        (1, 2),
        (2, 1),
    ]

    listener.clear()
    book.modify(2, 101, 4)
    assert book.best_bid() == 101
    book.submit(4, pylob.Side.Sell, pylob.OrderType.Limit, 101, 1)
    assert events_of(listener, pylob.Trade)[-1] == pylob.Trade(2, 4, 101, 1)


def test_queries_report_levels_best_first() -> None:
    book = make_book()
    book.submit(1, pylob.Side.Buy, pylob.OrderType.Limit, 99, 3)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 99, 2)
    book.submit(3, pylob.Side.Buy, pylob.OrderType.Limit, 98, 4)
    book.submit(4, pylob.Side.Sell, pylob.OrderType.Limit, 101, 1)

    assert book.best_bid() == 99
    assert book.best_ask() == 101
    assert book.level_qty(pylob.Side.Buy, 99) == 5
    assert book.level_qty(pylob.Side.Buy, 97) == 0
    assert book.order_count() == 4

    levels = book.top_levels(pylob.Side.Buy, 5)
    assert [(level.price, level.qty, level.order_count) for level in levels] == [
        (99, 5, 2),
        (98, 4, 1),
    ]
    assert [level.price for level in book.top_levels(pylob.Side.Sell, 1)] == [101]
    assert book.top_levels(pylob.Side.Sell, 0) == []
    assert book.digest() == book.digest()
    assert isinstance(book.digest(), int)


def test_rejections_follow_the_validation_order() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Buy, pylob.OrderType.Limit, 100, 0)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 0, 1)
    book.submit(3, pylob.Side.Buy, pylob.OrderType.Limit, 100, 1)
    listener.clear()

    book.submit(3, pylob.Side.Sell, pylob.OrderType.Limit, 101, 1)
    book.cancel(999)
    book.modify(999, 100, 1)
    book.modify(3, 100, 1)
    book.modify(3, 100, 0)

    assert listener.events == [
        pylob.Reject(3, pylob.RejectReason.DuplicateId),
        pylob.Reject(999, pylob.RejectReason.UnknownOrder),
        pylob.Reject(999, pylob.RejectReason.UnknownOrder),
        pylob.Reject(3, pylob.RejectReason.ModifyNoChange),
        pylob.Reject(3, pylob.RejectReason.InvalidQuantity),
    ]


def test_market_and_ioc_remainders_are_cancelled() -> None:
    listener = pylob.RecordingListener()
    book = make_book(listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 2)
    listener.clear()

    book.submit(2, pylob.Side.Buy, pylob.OrderType.Market, 0, 5)
    assert listener.events[-1] == pylob.Cancel(2, 3)
    assert book.best_ask() is None

    listener.clear()
    book.submit(3, pylob.Side.Buy, pylob.OrderType.IOC, 50, 1)
    assert listener.events == [
        pylob.Accept(3),
        pylob.Cancel(3, 1),
    ]
    assert book.order_count() == 0


def test_any_object_with_callbacks_works_as_a_listener() -> None:
    trades: list[tuple[int, int, int, int]] = []

    class TradesOnly:
        def on_trade(self, maker_id: int, taker_id: int, price: int, qty: int) -> None:
            trades.append((maker_id, taker_id, price, qty))

    listener = TradesOnly()
    book = pylob.OrderBook(make_config(), listener)
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 100, 2)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 100, 2)

    assert trades == [(1, 2, 100, 2)]
    assert isinstance(pylob.RecordingListener(), pylob.Listener)
    assert len(pylob.RecordingListener()) == 0


def test_config_defaults_and_validation() -> None:
    assert pylob.__version__ == "0.1.0"
    config = pylob.Config()
    assert (config.min_price, config.max_price) == (1, 200_000)
    assert (config.max_orders, config.max_levels) == (1_048_576, 200_000)
    assert "Config(" in repr(config)

    with pytest.raises(ValueError):
        pylob.OrderBook(pylob.Config(min_price=0))
    with pytest.raises(ValueError):
        pylob.OrderBook(pylob.Config(min_price=10, max_price=5))
    with pytest.raises(ValueError):
        pylob.OrderBook(pylob.Config(max_orders=0))
