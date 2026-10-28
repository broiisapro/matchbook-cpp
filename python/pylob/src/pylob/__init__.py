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
