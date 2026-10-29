"""Build and render the Phase 2/3 latency report (decision C59, issue #154).

Usage:
  uv run scripts/phase23_report.py --work-dir build/release/phase23-work \
      --json build/release/phase23.json --load-before 0.42 --load-after 0.31 \
      > build/release/phase23.md

The work directory is filled by `scripts/run_phase23_benchmarks.sh`:

  spsc-run1.txt ...      stdout of one `spsc_latency` invocation
  w2w-s1-run1.json ...   load generator report of one wire-to-wire run

`collect` reads those raw files plus the machine and toolchain it is running
on, keeps every repetition, picks the median run per series by p50 (upper
median, the same rule as `bench_report.py`) and writes one JSON document. The
Markdown rendered from it carries the environment table, both latency tables
and the caveats that make the numbers honest.

The module is importable for the tests; `build_report` takes the environment as
a dict so a test never shells out.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PERCENTILE_KEYS = ("p50_ns", "p90_ns", "p99_ns", "p999_ns", "max_ns")

SPSC_PATTERNS = {
    "timer": re.compile(
        r"^timer: granularity ([0-9.]+) ns \(step\), calibrated engine event ([0-9.]+) ns$",
        re.MULTILINE,
    ),
    "mode": re.compile(r"^mode: (\S+)$", re.MULTILINE),
    "messages": re.compile(r"^messages: (\d+), accepted: (\d+), events/s: ([0-9.]+)$", re.MULTILINE),
    "samples": re.compile(r"^samples: (\d+)", re.MULTILINE),
    "latency": re.compile(
        r"^latency ns: p50 (\d+), p90 (\d+), p99 (\d+), p99\.9 (\d+), max (\d+), mean ([0-9.]+)$",
        re.MULTILINE,
    ),
}

W2W_CAVEAT = (
    "Wire-to-wire is native and measured on localhost: every request crosses "
    "the kernel's TCP loopback path in both directions and waits on both "
    "processes' scheduling, so it is not a network round trip. The load "
    "generator is closed-loop - each session sends its next request only after "
    "the previous response - so a slow response delays the next request "
    "instead of queueing behind it; these percentiles understate latency under "
    "load (coordinated omission)."
)

RING_CAVEAT = (
    "The ring number is the enqueue-to-`on_accept` handoff inside one process: "
    "one thread pushes an event, the other pops it and runs the engine, and the "
    "listener timestamps the callback. It includes any wait for the consumer "
    "and the measured timer overhead subtraction, and it excludes network I/O. "
    "In `batchN` mode every sample is the per-op average over a batch of N "
    "events, so the tail is smoothed by N; `per_op` times each event alone."
)

MACHINE_CAVEAT = (
    "macOS has no core isolation or pinning, so scheduler noise is in the "
    "tails. These numbers are only meaningful when taken by "
    "`scripts/run_phase23_benchmarks.sh` on an otherwise idle machine (load "
    "average at or below `LOB_BENCH_MAX_LOAD`, no compilers, CI or Docker "
    "builds). Docker and container numbers are never published as latency "
    "results."
)


class ReportError(Exception):
    """Raised when the raw files do not look like a Phase 2/3 run."""


def parse_spsc_run(text: str, source: str) -> dict:
    """One `spsc_latency` stdout as a flat run record."""
    timer = SPSC_PATTERNS["timer"].search(text)
    mode = SPSC_PATTERNS["mode"].search(text)
    messages = SPSC_PATTERNS["messages"].search(text)
    samples = SPSC_PATTERNS["samples"].search(text)
    latency = SPSC_PATTERNS["latency"].search(text)
    if not all((timer, mode, messages, samples, latency)):
        raise ReportError(f"{source} is not an spsc_latency output")
    p50, p90, p99, p999, maximum, mean = latency.groups()
    return {
        "source": Path(source).name,
        "mode": mode.group(1),
        "granularity_ns": float(timer.group(1)),
        "calibrated_event_ns": float(timer.group(2)),
        "messages": int(messages.group(1)),
        "accepted": int(messages.group(2)),
        "events_per_second": float(messages.group(3)),
        "samples": int(samples.group(1)),
        "p50_ns": int(p50),
        "p90_ns": int(p90),
        "p99_ns": int(p99),
        "p999_ns": int(p999),
        "max_ns": int(maximum),
        "mean_ns": float(mean),
    }


def load_spsc_runs(work_dir: Path) -> list[dict]:
    paths = sorted(work_dir.glob("spsc-run*.txt"))
    if not paths:
        raise ReportError(f"no spsc-run*.txt files in {work_dir}; was the suite run?")
    return [parse_spsc_run(path.read_text(encoding="utf-8"), str(path)) for path in paths]


def load_w2w_runs(work_dir: Path) -> list[dict]:
    """Every load generator report, tagged with the sessions it ran."""
    runs = []
    for path in sorted(work_dir.glob("w2w-*-run*.json")):
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise ReportError(f"{path} is not valid JSON: {error}") from error
        if not isinstance(report.get("latency_ns"), dict):
            raise ReportError(f"{path} has no latency block; is it a lob_loadgen report?")
        latency = report["latency_ns"]
        missing = [key for key in PERCENTILE_KEYS if key not in latency]
        if missing:
            raise ReportError(f"{path} is missing {', '.join(missing)}")
        md = report.get("md") or {}
        runs.append(
            {
                "source": path.name,
                "sessions": int(report.get("sessions", 0)),
                "orders": int(report.get("orders_requested", 0)),
                "requests_per_second": float(report.get("requests_per_second", 0.0)),
                "elapsed_seconds": float(report.get("elapsed_seconds", 0.0)),
                "p50_ns": latency["p50_ns"],
                "p90_ns": latency["p90_ns"],
                "p99_ns": latency["p99_ns"],
                "p999_ns": latency["p999_ns"],
                "max_ns": latency["max_ns"],
                "mean_ns": latency.get("mean_ns"),
                "orders_without_response": int(report.get("orders_without_response", 0)),
                "unrecovered_gaps": int(
                    md.get("unrecovered_gaps", md.get("sequence_gaps_unrecovered", 0))
                ),
                "md_received": int(md.get("received", 0)),
            }
        )
    if not runs:
        raise ReportError(f"no w2w-*-run*.json files in {work_dir}; was the suite run?")
    return runs


def median_run(runs: list[dict]) -> dict:
    """The run whose p50_ns is the median across the runs (upper median)."""
    ordered = sorted(runs, key=lambda run: run["p50_ns"])
    return ordered[len(ordered) // 2]


def p50_spread(runs: list[dict]) -> list[int]:
    values = [run["p50_ns"] for run in runs]
    return [min(values), max(values)]


def series(runs: list[dict]) -> dict:
    median = median_run(runs)
    return {
        "runs": len(runs),
        "median": median,
        "p50_spread_ns": p50_spread(runs),
    }


def collect_environment() -> dict:
    """Machine, OS and toolchain, from the host the report is rendered on."""

    def query(*command: str) -> str:
        try:
            result = subprocess.run(command, capture_output=True, text=True, check=False)
        except OSError:
            return ""
        return result.stdout.strip()

    def value(*command: str) -> str:
        return query(*command) or "unknown"

    machine = {
        "model": value("sysctl", "-n", "hw.model"),
        "chip": value("sysctl", "-n", "machdep.cpu.brand_string"),
        "memory_bytes": value("sysctl", "-n", "hw.memsize"),
    }
    os_version = {
        "name": value("sw_vers", "-productName"),
        "version": value("sw_vers", "-productVersion"),
        "build": value("sw_vers", "-buildVersion"),
    }
    compiler = query("c++", "--version").splitlines()
    return {
        "machine": machine,
        "os": os_version,
        "compiler": compiler[0] if compiler else "unknown",
        "commit": value("git", "rev-parse", "HEAD"),
        "commit_dirty": bool(query("git", "status", "--porcelain")),
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


def build_report(work_dir: Path, environment: dict, load_before, load_after) -> dict:
    spsc_runs = load_spsc_runs(work_dir)
    w2w_runs = load_w2w_runs(work_dir)

    modes = sorted({run["mode"] for run in spsc_runs})
    if len(modes) > 1:
        raise ReportError(f"the ring runs disagree on the sampling mode: {', '.join(modes)}")
    granularities = sorted({run["granularity_ns"] for run in spsc_runs})
    if len(granularities) > 1:
        raise ReportError("the ring runs disagree on the timer granularity")

    groups: dict[int, list[dict]] = {}
    for run in w2w_runs:
        groups.setdefault(run["sessions"], []).append(run)

    for count, runs in groups.items():
        for run in runs:
            if run["orders_without_response"] != 0 or run["unrecovered_gaps"] != 0:
                raise ReportError(
                    f"{run['source']} is not a clean run "
                    f"({run['orders_without_response']} unanswered, "
                    f"{run['unrecovered_gaps']} unrecovered gap(s))"
                )

    report = {
        "schema_version": 1,
        "kind": "phase23_latency",
        "date": environment.get("date", "unknown"),
        "commit": environment.get("commit", "unknown"),
        "commit_dirty": bool(environment.get("commit_dirty", False)),
        "machine": environment.get("machine", {}),
        "os": environment.get("os", {}),
        "compiler": environment.get("compiler", "unknown"),
        "build_flags": environment.get("build_flags", "unknown"),
        "load_before_1m": load_before,
        "load_after_1m": load_after,
        "timer": {
            "granularity_ns": granularities[0],
            "mode": modes[0],
            "calibrated_event_ns": spsc_runs[0]["calibrated_event_ns"],
            "clock": "bench/timer.hpp monotonic counter",
        },
        "ring": {
            "messages": spsc_runs[0]["messages"],
            **series(spsc_runs),
        },
        "wire_to_wire": {
            "clock": "std::chrono::steady_clock",
            "orders": groups[sorted(groups)[0]][0]["orders"],
            "groups": [{"sessions": count, **series(groups[count])} for count in sorted(groups)],
        },
    }
    return report


def format_ns(value) -> str:
    return f"{round(value):,}"


def format_spread(spread) -> str:
    low, high = spread
    if low == high:
        return format_ns(low)
    return f"{format_ns(low)}–{format_ns(high)}"


def format_load(value) -> str:
    return "unknown" if value is None else f"{value:.2f}"


def mode_explanation(mode: str) -> str:
    if mode.startswith("batch"):
        return f"`{mode}`: every sample is the per-op average over {mode[5:]} events."
    return f"`{mode}`: every sample times one event."


def render(report: dict) -> str:
    ring = report["ring"]
    median = ring["median"]
    machine = report.get("machine", {})
    os_version = report.get("os", {})
    memory = machine.get("memory_bytes")
    try:
        gib = f"{int(memory) / (1 << 30):.0f} GiB"
    except (TypeError, ValueError):
        gib = "unknown"

    lines = [
        "# Phase 2/3 latency report",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| Date (UTC) | {report.get('date', 'unknown')} |",
        f"| Commit | `{report.get('commit', 'unknown')}`"
        + (" (working tree dirty)" if report.get("commit_dirty") else "")
        + " |",
        f"| Machine | {machine.get('model', 'unknown')}, {machine.get('chip', 'unknown')}, {gib} |",
        f"| OS | {os_version.get('name', 'unknown')} {os_version.get('version', 'unknown')}"
        f" ({os_version.get('build', 'unknown')}) |",
        f"| Compiler | {report.get('compiler', 'unknown')} |",
        f"| Build flags | `{report.get('build_flags', 'unknown')}` |",
        f"| Timer | granularity {report['timer'].get('granularity_ns')} ns per step |",
        f"| Ring sampling mode | `{report['timer'].get('mode')}` |",
        f"| Repetitions | {ring.get('runs')} ring run"
        f"{'' if ring.get('runs') == 1 else 's'}; "
        + "; ".join(
            f"{group['runs']} wire-to-wire run"
            f"{'' if group['runs'] == 1 else 's'} at {group['sessions']} session"
            f"{'' if group['sessions'] == 1 else 's'}"
            for group in report['wire_to_wire']['groups']
        )
        + " |",
        f"| Load average (1 min) | {format_load(report.get('load_before_1m'))} before, "
        f"{format_load(report.get('load_after_1m'))} after |",
        "",
        f"## SPSC ring: enqueue to `on_accept` (ns, median run of {ring.get('runs')})",
        "",
        "| Statistic | ns |",
        "| --- | --- |",
    ]
    for key, label in (
        ("p50_ns", "p50"),
        ("p90_ns", "p90"),
        ("p99_ns", "p99"),
        ("p999_ns", "p99.9"),
        ("max_ns", "max"),
    ):
        lines.append(f"| {label} | {format_ns(median[key])} |")
    lines += [
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| Events per run | {ring.get('messages'):,} |",
        f"| Samples (median run) | {median.get('samples'):,} |",
        f"| Throughput (median run) | {median.get('events_per_second'):,.0f} events/s |",
        f"| p50 across runs | {format_spread(ring['p50_spread_ns'])} ns |",
        "",
        f"> {mode_explanation(report['timer'].get('mode', 'unknown'))}",
        f"> {RING_CAVEAT}",
        "",
        "## Wire-to-wire, native closed loop on localhost (ns, median run per session count)",
        "",
    ]

    groups = report["wire_to_wire"]["groups"]
    header = "| Statistic | " + " | ".join(
        f"{group['sessions']} session" + ("" if group["sessions"] == 1 else "s") for group in groups
    ) + " |"
    lines += [header, "| --- | " + " | ".join("---" for _ in groups) + " |"]
    for key, label in (
        ("p50_ns", "p50"),
        ("p90_ns", "p90"),
        ("p99_ns", "p99"),
        ("p999_ns", "p99.9"),
        ("max_ns", "max"),
    ):
        cells = " | ".join(format_ns(group["median"][key]) for group in groups)
        lines.append(f"| {label} | {cells} |")
        if key == "p50_ns":
            spread = " | ".join(
                format_spread(group["p50_spread_ns"]) + " ns" for group in groups
            )
            lines.append(f"| p50 across runs | {spread} |")
    throughput = " | ".join(
        f"{group['median']['requests_per_second']:,.0f}" for group in groups
    )
    lines.append(f"| Requests/s (median run) | {throughput} |")
    lines.append(
        f"| Orders per run | "
        + " | ".join(f"{group['median']['orders']:,}" for group in groups)
        + " |"
    )
    lines.append(
        "| Unrecovered market-data gaps | "
        + " | ".join(str(group["median"]["unrecovered_gaps"]) for group in groups)
        + " |"
    )
    lines.append(
        f"| Runs | " + " | ".join(str(group["runs"]) for group in groups) + " |"
    )
    lines += [
        "",
        "> Wire-to-wire is timed per request with "
        f"`{report['wire_to_wire'].get('clock', 'std::chrono::steady_clock')}` in nanoseconds; "
        "the ring sampling mode above applies to the ring table only.",
        f"> {W2W_CAVEAT}",
        "",
        f"> {MACHINE_CAVEAT}",
        "",
    ]
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--work-dir", required=True, type=Path,
                        help="directory with spsc-run*.txt and w2w-*-run*.json")
    parser.add_argument("--json", required=True, type=Path,
                        help="destination of the combined JSON report")
    parser.add_argument("--load-before", type=float, default=None)
    parser.add_argument("--load-after", type=float, default=None)
    parser.add_argument("--build-flags", default="unknown",
                        help="compiler flags of the release build under test")
    arguments = parser.parse_args(argv)

    environment = collect_environment()
    environment["build_flags"] = arguments.build_flags
    try:
        report = build_report(
            arguments.work_dir, environment, arguments.load_before, arguments.load_after
        )
    except ReportError as error:
        print(f"phase23_report: {error}", file=sys.stderr)
        return 1

    arguments.json.parent.mkdir(parents=True, exist_ok=True)
    arguments.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(render(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
