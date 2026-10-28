"""Writes flow files for the pylob tests without depending on the C++ tools.

The wire format is the frozen one from docs/architecture.md section 6: a
32-byte header followed by fixed-size 40-byte little-endian records. Keeping
the writer here makes the Python tests self-contained while still exercising
the real C++ reader through pylob.
"""

from __future__ import annotations

import random
import struct
from collections.abc import Iterable, Sequence
from pathlib import Path

HEADER = "<8sIIQQ"
RECORD = "<BBB5xQqqQ"

MAGIC = b"LOBFLOW1"
VERSION = 1
RECORD_SIZE = 40

SUBMIT = 1
CANCEL = 2
MODIFY = 3

BUY = 0
SELL = 1

LIMIT = 0
MARKET = 1
IOC = 2

#: (op, side, type, id, price, qty, ts_ns)
Record = tuple[int, int, int, int, int, int, int]

_OP_WORDS = {SUBMIT: "submit", CANCEL: "cancel", MODIFY: "modify"}
_SIDE_WORDS = {BUY: "buy", SELL: "sell"}
_TYPE_WORDS = {LIMIT: "limit", MARKET: "market", IOC: "ioc"}


def write_flow(path: str | Path, records: Iterable[Record]) -> Path:
    """Writes a binary flow file and returns its path."""
    data = list(records)
    target = Path(path)

    with target.open("wb") as handle:
        handle.write(struct.pack(HEADER, MAGIC, VERSION, RECORD_SIZE, len(data), 0))
        for record in data:
            handle.write(struct.pack(RECORD, *record))
    return target


def write_csv_flow(path: str | Path, records: Sequence[Record]) -> Path:
    """Writes the CSV form of the same records and returns its path."""
    target = Path(path)
    lines = ["op,side,type,id,price,qty,ts_ns"]
    for op, side, type_, id_, price, qty, ts_ns in records:
        lines.append(
            f"{_OP_WORDS[op]},{_SIDE_WORDS[side]},{_TYPE_WORDS[type_]},"
            f"{id_},{price},{qty},{ts_ns}"
        )
    target.write_text("\n".join(lines) + "\n", encoding="ascii")
    return target


def sample_flow(
    orders: int = 2000,
    seed: int = 7,
    cancel_ratio: float = 0.15,
    mid: int = 10_000,
) -> list[Record]:
    """Builds a deterministic two-sided flow with occasional cancels.

    ``orders`` is the number of submit records; the result also holds cancels.
    Prices stay well inside the default replay configuration. Because the mid
    drifts and resting orders are left behind, some submits cross and trade,
    which keeps the replay summary interesting.
    """
    rng = random.Random(seed)
    records: list[Record] = []
    live: list[int] = []
    next_id = 1
    ts_ns = 0

    for index in range(orders):
        ts_ns += 1_000_000
        if live and rng.random() < cancel_ratio:
            order_id = live.pop(rng.randrange(len(live)))
            records.append((CANCEL, BUY, LIMIT, order_id, 0, 0, ts_ns))

        side = index % 2 if index < 2 else rng.choice((BUY, SELL))
        price = mid - rng.randint(1, 20) if side == BUY else mid + rng.randint(1, 20)
        quantity = rng.randint(1, 10)
        records.append((SUBMIT, side, LIMIT, next_id, price, quantity, ts_ns))
        live.append(next_id)
        next_id += 1

        if index % 40 == 39:
            mid = max(200, min(190_000, mid + rng.choice((-2, -1, 0, 1, 2))))

    return records
