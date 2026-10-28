#!/usr/bin/env python3
"""Replay a flow file through pylob and plot the bid/ask spread over time.

    uv run python examples/replay_spread.py --flow /tmp/flow.bin --out /tmp/spread.png

The flow is replayed one record at a time against a ``pylob.OrderBook``; every
``--every`` records, and on the last record, the best bid and ask are sampled
and the spread is computed. The PNG is written with matplotlib, which is an
optional dependency of the repository environment (the ``plot`` group).
"""

from __future__ import annotations

import argparse
import statistics
from collections.abc import Sequence
from pathlib import Path

import pylob


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay a flow file through pylob and plot the bid/ask spread."
    )
    parser.add_argument("--flow", required=True, type=Path, help="flow file (binary or .csv)")
    parser.add_argument("--out", required=True, type=Path, help="PNG file to write")
    parser.add_argument(
        "--every",
        type=int,
        default=100,
        metavar="N",
        help="sample the spread every N records (default: 100)",
    )
    parser.add_argument(
        "--title",
        default=None,
        help="plot title (default: the flow file name)",
    )
    args = parser.parse_args(argv)
    if args.every < 1:
        parser.error("--every must be at least 1")
    return args


def replay_and_sample(flow: Path, every: int) -> tuple[list[float], list[int], int, str]:
    """Replays the flow and returns (x values, spreads, record count, x label).

    The x axis is relative time in seconds when the flow carries advancing
    timestamps, and the record index otherwise.
    """
    records = pylob.read_flow(str(flow))
    book = pylob.OrderBook(pylob.Config())

    by_time = bool(records) and records[-1].ts_ns > records[0].ts_ns
    origin_ns = records[0].ts_ns if records else 0
    xs: list[float] = []
    spreads: list[int] = []

    for index, record in enumerate(records):
        if record.op == "submit":
            book.submit(record.id, record.side, record.type, record.price, record.qty)
        elif record.op == "cancel":
            book.cancel(record.id)
        elif record.op == "modify":
            book.modify(record.id, record.price, record.qty)
        else:  # pragma: no cover - the reader rejects unknown operations
            raise SystemExit(f"unsupported flow operation: {record.op!r}")

        if index % every == 0 or index == len(records) - 1:
            best_bid = book.best_bid()
            best_ask = book.best_ask()
            if best_bid is None or best_ask is None:
                continue
            xs.append((record.ts_ns - origin_ns) / 1e9 if by_time else float(index))
            spreads.append(best_ask - best_bid)

    return xs, spreads, len(records), "time (s)" if by_time else "flow record"


def plot_spread(
    xs: Sequence[float],
    spreads: Sequence[int],
    out: Path,
    title: str,
    x_label: str,
) -> None:
    try:
        import matplotlib
    except ImportError as exc:  # pragma: no cover - depends on the environment
        raise SystemExit(
            "matplotlib is required for plotting; install the plot group with: "
            "uv sync --group plot"
        ) from exc

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(figsize=(10.0, 4.5), dpi=150)
    axes.plot(xs, spreads, color="#1f5fa8", linewidth=1.2)
    axes.fill_between(xs, spreads, color="#1f5fa8", alpha=0.12)
    axes.set_xlabel(x_label)
    axes.set_ylabel("spread (ticks)")
    axes.set_title(title)
    axes.set_ylim(bottom=0)
    axes.grid(True, color="#d0d5dd", linewidth=0.6, alpha=0.7)
    for spine in ("top", "right"):
        axes.spines[spine].set_visible(False)
    figure.tight_layout()

    out.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(out)
    plt.close(figure)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    xs, spreads, record_count, x_label = replay_and_sample(args.flow, args.every)

    if not spreads:
        raise SystemExit("the flow never had both a best bid and a best ask; nothing to plot")

    title = args.title or f"bid/ask spread: {args.flow.name}"
    plot_spread(xs, spreads, args.out, title, x_label)

    print(f"records: {record_count}")
    print(f"samples: {len(spreads)}")
    print(
        "spread_ticks: "
        f"min={min(spreads)} mean={statistics.fmean(spreads):.2f} max={max(spreads)}"
    )
    print(f"out: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
