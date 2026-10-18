"""Render a Google Benchmark JSON report from lob_bench as Markdown.

Usage: uv run scripts/bench_report.py build/release/bench.json > bench.md

For every expected benchmark the table shows the sampling mode and the latency
percentiles of the median run across repetitions (selected by p50_ns), plus the
spread of p50 across runs. In `batchN` mode each sample is the per-op average
over a batch of N operations, so the percentiles describe per-op latency with
the tail smoothed by N (decision C32). Mixed flow adds a throughput row.
Benchmarks that were skipped because the engine could not run them are shown as
skipped; a benchmark that ran but is missing one of the
p50_ns/p90_ns/p99_ns/p999_ns/max_ns/mode/batch_size counters fails with a clear
error instead of being silently dropped.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PERCENTILE_KEYS = ("p50_ns", "p90_ns", "p99_ns", "p999_ns", "max_ns")
REQUIRED_COUNTERS = PERCENTILE_KEYS + ("mode", "batch_size")

EXPECTED_BENCHMARKS = (
    "AddOrder_NonCrossing",
    "AddOrder_Crossing/1",
    "AddOrder_Crossing/2",
    "AddOrder_Crossing/3",
    "Cancel_Resting",
    "Market_Sweep/1",
    "Market_Sweep/2",
    "Market_Sweep/3",
    "MixedFlow",
)

THROUGHPUT_BENCHMARK = "MixedFlow"

CAVEAT = (
    "macOS has no core isolation or pinning; the QoS hint "
    "(`pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`) is the "
    "only scheduling control, so the tails include scheduler noise. Numbers "
    "are only meaningful when taken by `scripts/run_benchmarks.sh` on an "
    "otherwise idle machine."
)

BATCH_EXPLANATION = (
    "`batchN` mode: every percentile is of per-op averages over batches of N "
    "operations, so the tails are smoothed by N; `per_op` times each operation "
    "individually."
)

PER_OP_EXPLANATION = "`per_op` mode: every sample times one operation individually."


class ReportError(Exception):
    """Raised when the JSON does not look like a lob_bench report."""


def display_name(name: str) -> str:
    if name == "AddOrder_NonCrossing":
        return "Add order, non-crossing"
    if name == "Cancel_Resting":
        return "Cancel, resting"
    if name == "MixedFlow":
        return "Mixed flow"
    crossing = re.fullmatch(r"AddOrder_Crossing/(\d+)", name)
    if crossing:
        levels = crossing.group(1)
        return f"Add order, crossing {levels} level" + ("" if levels == "1" else "s")
    market = re.fullmatch(r"Market_Sweep/(\d+)", name)
    if market:
        levels = market.group(1)
        return f"Market order, sweep {levels} level" + ("" if levels == "1" else "s")
    return name


def load_report(path: Path) -> dict:
    try:
        with path.open(encoding="utf-8") as handle:
            report = json.load(handle)
    except OSError as error:
        raise ReportError(f"cannot read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ReportError(f"{path} is not valid JSON: {error}") from error
    if not isinstance(report, dict) or "benchmarks" not in report:
        raise ReportError(f"{path} has no 'benchmarks' array; is it Google Benchmark JSON?")
    return report


def iteration_entries(report: dict) -> dict[str, list[dict]]:
    """Groups per-repetition entries by benchmark name."""
    groups: dict[str, list[dict]] = {}
    for entry in report["benchmarks"]:
        if entry.get("run_type") != "iteration":
            continue
        groups.setdefault(entry["name"], []).append(entry)
    return groups


def check_counters(name: str, entries: list[dict]) -> None:
    """A benchmark that ran must carry every required counter."""
    for entry in entries:
        if entry.get("iterations", 0) <= 0:
            continue
        missing = [key for key in REQUIRED_COUNTERS if key not in entry]
        if missing:
            repetition = entry.get("repetition_index", "?")
            raise ReportError(
                f"benchmark '{name}' repetition {repetition} is missing counters "
                f"{', '.join(missing)}; was this JSON written by lob_bench?"
            )


def median_run(entries: list[dict]) -> dict:
    """The run whose p50_ns is the median across the runs (upper median)."""
    running = sorted(
        (entry for entry in entries if entry.get("iterations", 0) > 0),
        key=lambda entry: entry["p50_ns"],
    )
    return running[len(running) // 2]


def skipped_reason(entries: list[dict]) -> str:
    if not entries:
        return "not run"
    return "skipped"


def format_ns(value: float) -> str:
    return f"{round(value):,}"


def format_spread(entries: list[dict]) -> str:
    running = [entry for entry in entries if entry.get("iterations", 0) > 0]
    if not running:
        return "—"
    values = [entry["p50_ns"] for entry in running]
    low, high = min(values), max(values)
    if low == high:
        return f"{format_ns(low)}"
    return f"{format_ns(low)}–{format_ns(high)}"


def mode_label(entry: dict) -> str:
    """The sampling mode of one benchmark result: per_op or batchN."""
    batch_size = entry.get("batch_size")
    if batch_size is None:
        return "unknown"
    size = int(batch_size)
    return "per_op" if size <= 1 else f"batch{size}"


def mode_explanation(groups: dict[str, list[dict]]) -> str:
    """One line explaining the mode labels used by the latency table."""
    batch_sizes = sorted(
        {
            int(entry["batch_size"])
            for entries in groups.values()
            for entry in entries
            if entry.get("iterations", 0) > 0 and entry.get("batch_size", 1) > 1
        }
    )
    if batch_sizes:
        sizes = ", ".join(str(size) for size in batch_sizes)
        return f"{BATCH_EXPLANATION} Batch sizes in this run: {sizes}."
    return PER_OP_EXPLANATION


def context_row(context: dict, key: str) -> str:
    value = context.get(key)
    return str(value) if value not in (None, "") else "unknown"


def render_header(report: dict, repetitions: int) -> list[str]:
    context = report.get("context", {})
    return [
        "# Benchmark report",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| CPU | {context_row(context, 'cpu')} |",
        f"| OS | {context_row(context, 'os')} |",
        f"| Compiler | {context_row(context, 'compiler')} |",
        f"| Build flags | {context_row(context, 'build_flags')} |",
        f"| Repetitions | {repetitions} |",
        f"| Timer | {context_row(context, 'timer')} |",
        f"| Timer granularity | {context_row(context, 'timer_granularity_ns')} ns |",
        f"| Latency mode | {context_row(context, 'latency_mode')} |",
        f"| Latency mode request | {context_row(context, 'latency_mode_request')} |",
        f"| Timer overhead | {context_row(context, 'timer_overhead_ns')} ns |",
        "",
        f"> {CAVEAT}",
        "",
    ]


def render_latency_table(groups: dict[str, list[dict]]) -> list[str]:
    names = list(EXPECTED_BENCHMARKS)
    names += [name for name in sorted(groups) if name not in EXPECTED_BENCHMARKS]
    lines = [
        "## Latency (ns, median run per benchmark)",
        "",
        "| Benchmark | Mode | p50 | p90 | p99 | p99.9 | max | p50 across runs |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in names:
        entries = groups.get(name, [])
        check_counters(name, entries)
        if not any(entry.get("iterations", 0) > 0 for entry in entries):
            lines.append(
                f"| {display_name(name)} | — | — | — | — | — | — | {skipped_reason(entries)} |"
            )
            continue
        run = median_run(entries)
        cells = " | ".join(format_ns(run[key]) for key in PERCENTILE_KEYS)
        lines.append(
            f"| {display_name(name)} | {mode_label(run)} | {cells} | {format_spread(entries)} |"
        )
    lines += ["", f"> {mode_explanation(groups)}", ""]
    return lines


def render_throughput_table(groups: dict[str, list[dict]]) -> list[str]:
    lines = [
        "## Throughput",
        "",
        "| Benchmark | ops/s (median run) | ops/s across runs |",
        "| --- | --- | --- |",
    ]
    entries = groups.get(THROUGHPUT_BENCHMARK, [])
    check_counters(THROUGHPUT_BENCHMARK, entries)
    running = [entry for entry in entries if entry.get("iterations", 0) > 0]
    if not running:
        lines.append(f"| {display_name(THROUGHPUT_BENCHMARK)} | — | {skipped_reason(entries)} |")
    else:
        run = median_run(entries)
        values = [entry["items_per_second"] for entry in running]
        low, high = min(values), max(values)
        spread = f"{low:,.0f}" if low == high else f"{low:,.0f}–{high:,.0f}"
        lines.append(f"| {display_name(THROUGHPUT_BENCHMARK)} | {run['items_per_second']:,.0f} | {spread} |")
    lines.append("")
    return lines


def render(report: dict) -> str:
    groups = iteration_entries(report)
    repetitions = max(
        (entry.get("repetitions", 1) for entries in groups.values() for entry in entries),
        default=1,
    )
    lines = render_header(report, repetitions)
    lines += render_latency_table(groups)
    lines += render_throughput_table(groups)
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <bench.json>", file=sys.stderr)
        return 2
    try:
        report = load_report(Path(argv[1]))
        sys.stdout.write(render(report))
    except ReportError as error:
        print(f"bench_report: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
