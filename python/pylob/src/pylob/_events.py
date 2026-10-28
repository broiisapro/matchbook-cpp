"""Event objects and listeners for the pylob bindings.

The engine calls a listener synchronously for every state change. Any object
with the matching method names works; the methods it does not define are
skipped. ``Listener`` is an empty base class for readability, and
``RecordingListener`` records every event as a small immutable object.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import ClassVar, TypeAlias

from ._core import RejectReason, Side

__all__ = [
    "Accept",
    "BookUpdate",
    "Cancel",
    "Event",
    "Listener",
    "Modify",
    "RecordingListener",
    "Reject",
    "Trade",
]


class Listener:
    """Base class for engine listeners; every callback is optional.

    Subclass it and override the callbacks you need. Callbacks run inside
    ``submit``/``cancel``/``modify`` on the calling thread (the GIL stays
    held), and a Python callback per event is much slower than running
    without a listener. A callback must not call back into the book.
    """

    def on_accept(self, id: int) -> None:  # noqa: A002 - matches the engine's field name
        """The order passed validation and enters the book."""

    def on_reject(self, id: int, reason: RejectReason) -> None:  # noqa: A002
        """The order was rejected; nothing else is emitted for it."""

    def on_trade(self, maker_id: int, taker_id: int, price: int, qty: int) -> None:
        """A resting order was filled by a taker at the resting price."""

    def on_cancel(self, id: int, cancelled_qty: int) -> None:  # noqa: A002
        """The remaining quantity of an order left the book without trading."""

    def on_modify(self, id: int, price: int, new_qty: int) -> None:  # noqa: A002
        """An in-place quantity reduction at the same price and queue position."""

    def on_book_update(self, side: Side, price: int, new_level_qty: int) -> None:
        """A price level changed; a level quantity of 0 means the level is gone."""


@dataclass(frozen=True, slots=True)
class Accept:
    """The order passed validation and enters the book."""

    id: int
    kind: ClassVar[str] = "accept"


@dataclass(frozen=True, slots=True)
class Reject:
    """The order was rejected; nothing else is emitted for it."""

    id: int
    reason: RejectReason
    kind: ClassVar[str] = "reject"


@dataclass(frozen=True, slots=True)
class Trade:
    """A resting (maker) order was filled by a taker at the resting price."""

    maker_id: int
    taker_id: int
    price: int
    qty: int
    kind: ClassVar[str] = "trade"


@dataclass(frozen=True, slots=True)
class Cancel:
    """Remaining quantity left the book without trading."""

    id: int
    cancelled_qty: int
    kind: ClassVar[str] = "cancel"


@dataclass(frozen=True, slots=True)
class Modify:
    """An in-place quantity reduction; the order keeps its queue position."""

    id: int
    price: int
    new_qty: int
    kind: ClassVar[str] = "modify"


@dataclass(frozen=True, slots=True)
class BookUpdate:
    """A price level changed; new_level_qty == 0 means the level is gone."""

    side: Side
    price: int
    new_level_qty: int
    kind: ClassVar[str] = "book_update"


Event: TypeAlias = Accept | Reject | Trade | Cancel | Modify | BookUpdate


class RecordingListener(Listener):
    """Collects every event in order in ``events``.

    >>> listener = RecordingListener()
    >>> book = OrderBook(Config(), listener)
    >>> book.submit(1, Side.Sell, OrderType.Limit, 100, 5)
    >>> listener.events
    [Accept(id=1), BookUpdate(side=<Side.Sell: 1>, price=100, new_level_qty=5)]
    """

    def __init__(self) -> None:
        self.events: list[Event] = []

    def clear(self) -> None:
        """Drops every recorded event."""
        self.events.clear()

    def __len__(self) -> int:
        return len(self.events)

    def on_accept(self, id: int) -> None:  # noqa: A002
        self.events.append(Accept(id))

    def on_reject(self, id: int, reason: RejectReason) -> None:  # noqa: A002
        self.events.append(Reject(id, reason))

    def on_trade(self, maker_id: int, taker_id: int, price: int, qty: int) -> None:
        self.events.append(Trade(maker_id, taker_id, price, qty))

    def on_cancel(self, id: int, cancelled_qty: int) -> None:  # noqa: A002
        self.events.append(Cancel(id, cancelled_qty))

    def on_modify(self, id: int, price: int, new_qty: int) -> None:  # noqa: A002
        self.events.append(Modify(id, price, new_qty))

    def on_book_update(self, side: Side, price: int, new_level_qty: int) -> None:
        self.events.append(BookUpdate(side, price, new_level_qty))
