"""Render a Google Benchmark JSON report from lob_bench as Markdown.

Usage:
  uv run scripts/bench_report.py build/release/bench.json > bench.md
  uv run scripts/bench_report.py --before build/release-tree/bench.json \
      --after build/release/bench.json > bench-compare.md
  uv run scripts/bench_report.py --merge build/release/bench.json round0.json round1.json

The single-report mode shows the sampling mode and the latency percentiles of
the median run across repetitions (selected by p50_ns), plus the spread of p50
across runs. In `batchN` mode each sample is the per-op average over a batch of
N operations, so the percentiles describe per-op latency with the tail smoothed
by N (decision C32). Mixed flow adds a throughput row.

The comparison mode shows both builds' shared environment, before/after
p50/p99/p99.9 with the after/before ratio, and MixedFlow throughput with the
same ratio. The merge mode combines single-repetition JSON files into one
report with unique repetition indices, which is how `run_benchmarks.sh
--compare` interleaves its rounds.

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

COMPARISON_CAVEAT = (
    "Ratios are after/before: below 1.00 means the after build is faster on "
    "that statistic. Both sides must come from one "
    "`scripts/run_benchmarks.sh --compare` run on an otherwise idle machine; "
    "numbers taken while builds, tests or CI share the machine are noise."
)

BATCH_EXPLANATION = (
    "`batchN` mode: every percentile is of per-op averages over batches of N "
    "operations, so the tails are smoothed by N; `per_op` times each operation "
    "individually."
)

PER_OP_EXPLANATION = "`per_op` mode: every sample times one operation individually."

COMPARISON_CONTEXT_ROWS = (
    ("cpu", "CPU"),
    ("os", "OS"),
    ("compiler", "Compiler"),
    ("build_flags", "Build flags"),
    ("price_ladder", "Price ladder"),
    ("timer", "Timer"),
    ("timer_granularity_ns", "Timer granularity"),
    ("latency_mode", "Latency mode"),
)


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


def report_repetitions(groups: dict[str, list[dict]]) -> int:
    return max(
        (entry.get("repetitions", 1) for entries in groups.values() for entry in entries),
        default=1,
    )


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
        lines.append(
            f"| {display_name(THROUGHPUT_BENCHMARK)} | {run['items_per_second']:,.0f} | {spread} |"
        )
    lines.append("")
    return lines


def render(report: dict) -> str:
    groups = iteration_entries(report)
    lines = render_header(report, report_repetitions(groups))
    lines += render_latency_table(groups)
    lines += render_throughput_table(groups)
    return "\n".join(lines)


def ratio(after_value: float, before_value: float) -> str:
    if not before_value:
        return "—"
    return f"{after_value / before_value:.2f}×"


def render_comparison_header(before: dict, after: dict) -> list[str]:
    before_context = before.get("context", {})
    after_context = after.get("context", {})
    lines = [
        "# Benchmark comparison (before vs after)",
        "",
        "| Field | Before | After |",
        "| --- | --- | --- |",
    ]
    for key, label in COMPARISON_CONTEXT_ROWS:
        lines.append(
            f"| {label} | {context_row(before_context, key)} | {context_row(after_context, key)} |"
        )
    lines.append(
        "| Repetitions | "
        f"{report_repetitions(iteration_entries(before))} | "
        f"{report_repetitions(iteration_entries(after))} |"
    )
    lines += ["", f"> {CAVEAT}", ""]
    return lines


def comparison_names(before_groups: dict, after_groups: dict) -> list[str]:
    names = [name for name in EXPECTED_BENCHMARKS if name in before_groups or name in after_groups]
    extras = {
        name
        for name in set(before_groups) | set(after_groups)
        if name not in EXPECTED_BENCHMARKS
    }
    return names + sorted(extras)


def render_comparison_latency_table(before_groups: dict, after_groups: dict) -> list[str]:
    lines = [
        "## Latency (ns, median run per build)",
        "",
        "| Benchmark | p50 before | p50 after | p50 ratio | p99 before | p99 after | p99 ratio "
        "| p99.9 before | p99.9 after | p99.9 ratio |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in comparison_names(before_groups, after_groups):
        before_entries = before_groups.get(name, [])
        after_entries = after_groups.get(name, [])
        check_counters(name, before_entries)
        check_counters(name, after_entries)
        before_running = [entry for entry in before_entries if entry.get("iterations", 0) > 0]
        after_running = [entry for entry in after_entries if entry.get("iterations", 0) > 0]
        if not before_running or not after_running:
            cells = []
            for key in ("p50_ns", "p99_ns", "p999_ns"):
                if before_running:
                    cells.append(format_ns(median_run(before_entries)[key]))
                else:
                    cells.append(skipped_reason(before_entries))
                if after_running:
                    cells.append(format_ns(median_run(after_entries)[key]))
                else:
                    cells.append(skipped_reason(after_entries))
                cells.append("—")
            lines.append(f"| {display_name(name)} | {' | '.join(cells)} |")
            continue
        before_run = median_run(before_entries)
        after_run = median_run(after_entries)
        cells = []
        for key in ("p50_ns", "p99_ns", "p999_ns"):
            cells.append(format_ns(before_run[key]))
            cells.append(format_ns(after_run[key]))
            cells.append(ratio(after_run[key], before_run[key]))
        lines.append(f"| {display_name(name)} | {' | '.join(cells)} |")
    lines.append("")
    return lines


def render_comparison_throughput_table(before_groups: dict, after_groups: dict) -> list[str]:
    lines = [
        "## Throughput",
        "",
        "| Benchmark | Before ops/s | After ops/s | Ratio (after/before) |",
        "| --- | --- | --- | --- |",
    ]
    before_entries = before_groups.get(THROUGHPUT_BENCHMARK, [])
    after_entries = after_groups.get(THROUGHPUT_BENCHMARK, [])
    check_counters(THROUGHPUT_BENCHMARK, before_entries)
    check_counters(THROUGHPUT_BENCHMARK, after_entries)
    before_running = [entry for entry in before_entries if entry.get("iterations", 0) > 0]
    after_running = [entry for entry in after_entries if entry.get("iterations", 0) > 0]
    name = display_name(THROUGHPUT_BENCHMARK)
    if not before_running or not after_running:
        before_cell = (
            f"{median_run(before_entries)['items_per_second']:,.0f}"
            if before_running
            else skipped_reason(before_entries)
        )
        after_cell = (
            f"{median_run(after_entries)['items_per_second']:,.0f}"
            if after_running
            else skipped_reason(after_entries)
        )
        lines.append(f"| {name} | {before_cell} | {after_cell} | — |")
    else:
        before_value = median_run(before_entries)["items_per_second"]
        after_value = median_run(after_entries)["items_per_second"]
        lines.append(
            f"| {name} | {before_value:,.0f} | {after_value:,.0f} | "
            f"{ratio(after_value, before_value)} |"
        )
    lines += ["", f"> {COMPARISON_CAVEAT}", ""]
    return lines


def render_comparison(before: dict, after: dict) -> str:
    before_groups = iteration_entries(before)
    after_groups = iteration_entries(after)
    lines = render_comparison_header(before, after)
    lines += render_comparison_latency_table(before_groups, after_groups)
    lines += render_comparison_throughput_table(before_groups, after_groups)
    return "\n".join(lines)


def merge_reports(paths: list[Path]) -> dict:
    """Combines reports into one report with contiguous repetition indices.

    The merged report has one entry per input repetition; the inputs' own
    repetition indices are offset so they stay unique, and every entry carries
    the total repetition count. Aggregate entries are dropped because the
    consumer recomputes them from the iteration entries. The first input's
    context is kept, with the last input's date when present.
    """
    if not paths:
        raise ReportError("--merge needs at least one input report")
    reports = [load_report(path) for path in paths]
    offsets: list[int] = []
    total_repetitions = 0
    for report in reports:
        offsets.append(total_repetitions)
        total_repetitions += report_repetitions(iteration_entries(report))
    entries: list[dict] = []
    for index, report in enumerate(reports):
        for entry in report["benchmarks"]:
            if entry.get("run_type") != "iteration":
                continue
            merged_entry = dict(entry)
            merged_entry["repetition_index"] = offsets[index] + int(entry.get("repetition_index", 0))
            merged_entry["repetitions"] = total_repetitions
            entries.append(merged_entry)
    context = dict(reports[0].get("context", {}))
    last_context = reports[-1].get("context", {})
    if "date" in last_context:
        context["date"] = last_context["date"]
    return {"context": context, "benchmarks": entries}


def write_report(report: dict, path: Path) -> None:
    try:
        with path.open("w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
    except OSError as error:
        raise ReportError(f"cannot write {path}: {error}") from error


USAGE = (
    f"usage: {sys.argv[0]} <bench.json>\n"
    f"       {sys.argv[0]} --before <bench.json> --after <bench.json>\n"
    f"       {sys.argv[0]} --merge <out.json> <bench.json> [<bench.json> ...]"
)


def main(argv: list[str]) -> int:
    args = argv[1:]
    try:
        if args[:1] == ["--before"]:
            if len(args) != 4 or args[2] != "--after":
                print(USAGE, file=sys.stderr)
                return 2
            before = load_report(Path(args[1]))
            after = load_report(Path(args[3]))
            sys.stdout.write(render_comparison(before, after))
            return 0
        if args[:1] == ["--merge"]:
            if len(args) < 3:
                print(USAGE, file=sys.stderr)
                return 2
            write_report(merge_reports([Path(argument) for argument in args[2:]]), Path(args[1]))
            return 0
        if len(args) == 1 and not args[0].startswith("--"):
            report = load_report(Path(args[0]))
            sys.stdout.write(render(report))
            return 0
    except ReportError as error:
        print(f"bench_report: error: {error}", file=sys.stderr)
        return 1
    print(USAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
