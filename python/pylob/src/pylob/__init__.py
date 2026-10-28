"""Python bindings for the matchbook-cpp limit order book and matching engine.

The engine is the same C++20 implementation the ``lobsim`` CLI uses: integer
prices in ticks, integer quantities in lots, price-time priority and trades at
the resting price.

    >>> import pylob
    >>> book = pylob.OrderBook(pylob.Config(min_price=1, max_price=100_000))
    >>> book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 10_005, 3)
    >>> book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 10_010, 2)
    >>> book.best_ask()
    10005

``pylob.replay`` replays a flow file through either engine and returns the
summary ``lobsim replay`` prints; ``pylob.read_flow`` exposes the records of a
flow file for tools that need to drive the book one record at a time.

``pylob.OrderType`` and ``pylob.RejectReason`` mirror ``lob/types.hpp``: the
member names are the CamelCase form of the engine's ``to_string`` words (the
same words the CSV flow format uses) and the integer values are the flow-file
bytes, so ``OrderType.FOK.value == 3`` and ``OrderType.PostOnly.value == 4``.
FOK and post-only follow the Phase 2 contract in ``docs/architecture.md``
sections 4.1.1 and 4.1.2; an engine that has not implemented them yet rejects
them with ``RejectReason.UnsupportedOrderType`` before any other validation.
"""

from __future__ import annotations

from ._core import (
    Config,
    FlowRecord,
    LevelInfo,
    OrderBook,
    OrderType,
    RejectReason,
    Side,
    read_flow,
    replay,
)
from ._events import (
    Accept,
    BookUpdate,
    Cancel,
    Event,
    Listener,
    Modify,
    RecordingListener,
    Reject,
    Trade,
)

__version__ = "0.1.0"

__all__ = [
    "Accept",
    "BookUpdate",
    "Cancel",
    "Config",
    "Event",
    "FlowRecord",
    "LevelInfo",
    "Listener",
    "Modify",
    "OrderBook",
    "OrderType",
    "RecordingListener",
    "Reject",
    "RejectReason",
    "Side",
    "Trade",
    "__version__",
    "read_flow",
    "replay",
]
