"""Tests for scripts/bench_report.py."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).resolve().parent
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import bench_report  # noqa: E402  (import after sys.path setup)

SAMPLE = REPO / "bench" / "testdata" / "sample_bench.json"


def make_entry(
    name: str,
    repetition: int,
    p50: float,
    p90: float,
    p99: float,
    p999: float,
    max_ns: float,
    *,
    iterations: int = 100,
    items: float | None = None,
    mode: float = 0.0,
) -> dict:
    return {
        "name": name,
        "run_type": "iteration",
        "repetitions": 3,
        "repetition_index": repetition,
        "iterations": iterations,
        "items_per_second": items,
        "mode": mode,
        "p50_ns": p50,
        "p90_ns": p90,
        "p99_ns": p99,
        "p999_ns": p999,
        "max_ns": max_ns,
    }


def minimal_report(entries: list[dict], context: dict | None = None) -> dict:
    return {"context": context or {}, "benchmarks": entries}


def test_renders_committed_sample() -> None:
    report = bench_report.load_report(SAMPLE)
    text = bench_report.render(report)
    assert "# Benchmark report" in text
    for field in ("| CPU |", "| OS |", "| Compiler |", "| Build flags |", "| Timer |",
                  "| Latency mode |", "| Repetitions |"):
        assert field in text
    assert bench_report.CAVEAT in text
    for name in bench_report.EXPECTED_BENCHMARKS:
        assert bench_report.display_name(name) in text


def test_median_run_selected_by_p50() -> None:
    entries = [
        make_entry("AddOrder_NonCrossing", 0, 300, 310, 320, 330, 400),
        make_entry("AddOrder_NonCrossing", 1, 200, 210, 220, 230, 999),
        make_entry("AddOrder_NonCrossing", 2, 100, 110, 120, 130, 150),
    ]
    text = bench_report.render(minimal_report(entries))
    rows = [line for line in text.splitlines() if line.startswith("| Add order, non-crossing")]
    assert len(rows) == 1
    assert "| 200 |" in rows[0]
    assert "999" in rows[0]  # max comes from the median run, not the fastest one
    assert "100–300" in rows[0]


def test_missing_counters_fail_with_clear_message() -> None:
    entry = make_entry("Cancel_Resting", 0, 100, 110, 120, 130, 150)
    del entry["p90_ns"]
    with pytest.raises(bench_report.ReportError, match=r"Cancel_Resting.*p90_ns"):
        bench_report.render(minimal_report([entry]))


def test_skipped_benchmark_is_rendered_as_skipped() -> None:
    entry = {
        "name": "MixedFlow",
        "run_type": "iteration",
        "repetitions": 2,
        "repetition_index": 0,
        "iterations": 0,
    }
    text = bench_report.render(minimal_report([entry]))
    rows = [line for line in text.splitlines() if line.startswith("| Mixed flow")]
    assert rows and "skipped" in rows[0]


def test_mixed_flow_throughput_row() -> None:
    entries = [
        make_entry("MixedFlow", 0, 100, 110, 120, 130, 150, items=1_000_000.0),
        make_entry("MixedFlow", 1, 200, 210, 220, 230, 250, items=2_000_000.0),
        make_entry("MixedFlow", 2, 50, 60, 70, 80, 90, items=500_000.0),
    ]
    text = bench_report.render(minimal_report(entries))
    rows = [line for line in text.splitlines() if line.startswith("| Mixed flow |")]
    assert rows and "1,000,000" in rows[-1]


def test_sample_percentiles_are_monotonic_and_consistent() -> None:
    """Every running benchmark in the committed sample must have sane latency
    accounting.

    Monotonicity and non-negativity hold for every mode. For per-op mode (the
    only mode a >= 1 GHz counter selects, and the mode of the committed
    sample) one iteration is one operation, so the samples' mean must sit
    inside the iteration wall time: a harness that measured the wrong region
    or lost samples would break one of the two bounds. The stated tolerance is
    factor 4 with 200 ns of slack on the upper side, which covers the harness's
    own per-iteration overhead and samples capped at the 10 ms HDR ceiling
    during a noisy run.
    """
    report = bench_report.load_report(SAMPLE)
    running = 0
    for entries in bench_report.iteration_entries(report).values():
        for entry in entries:
            if entry.get("iterations", 0) <= 0:
                continue
            running += 1
            counters = [entry[key] for key in bench_report.PERCENTILE_KEYS]
            assert all(value >= 0 for value in counters), entry["name"]
            assert counters == sorted(counters), entry["name"]  # p50 <= ... <= max
            if entry.get("mode") == 0.0:
                mean_ns = entry["mean_ns"]
                real_time = entry["real_time"]
                assert 0 < mean_ns <= real_time * 1.1 + 100.0, entry["name"]
                assert real_time <= mean_ns * 4.0 + 200.0, entry["name"]
    assert running == len(bench_report.EXPECTED_BENCHMARKS) * 5

