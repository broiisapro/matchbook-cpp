"""Tests for scripts/phase23_report.py."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

import phase23_report  # noqa: E402  (import after sys.path setup)

SPSC_TEXT = """spsc_latency: enqueue to on_accept handoff latency
source: seeded operation generator, seed 1, default mix
messages: 1000000, accepted: 999999, events/s: 1234567
ring capacity: 256
timer: granularity 41.7 ns (step), calibrated engine event 12.3 ns
mode: batch64
samples: 15625
latency ns: p50 123, p90 456, p99 789, p99.9 1000, max 2000, mean 150.0
note: noisy run on a shared machine; an exploratory measurement, not a publishable number
"""


def make_w2w(sessions: int, p50: int, *, unanswered: int = 0, gaps: int = 0) -> dict:
    return {
        "sessions": sessions,
        "orders_requested": 200000,
        "requests_per_second": 12345.6,
        "elapsed_seconds": 16.2,
        "orders_without_response": unanswered,
        "latency_ns": {
            "count": 200000,
            "p50_ns": p50,
            "p90_ns": p50 * 2,
            "p99_ns": p50 * 3,
            "p999_ns": p50 * 4,
            "max_ns": p50 * 10,
            "mean_ns": float(p50),
        },
        "md": {"received": 318000, "sequence_gaps_unrecovered": gaps},
    }


def write_spsc(
    work: Path, index: int, p50: int, mode: str = "batch64", granularity: str = "41.7"
) -> None:
    text = (
        SPSC_TEXT.replace("p50 123", f"p50 {p50}")
        .replace("mode: batch64", f"mode: {mode}")
        .replace("granularity 41.7", f"granularity {granularity}")
    )
    (work / f"spsc-run{index}.txt").write_text(text, encoding="utf-8")


def write_w2w(work: Path, sessions: int, index: int, p50: int, **kwargs) -> None:
    (work / f"w2w-s{sessions}-run{index}.json").write_text(
        json.dumps(make_w2w(sessions, p50, **kwargs)), encoding="utf-8"
    )


def fill_work_dir(work: Path) -> None:
    for index, p50 in enumerate((300, 100, 200, 400, 500), start=1):
        write_spsc(work, index, p50)
    for index, p50 in enumerate((9000, 7000, 8000, 10000, 11000), start=1):
        write_w2w(work, 1, index, p50)
    for index, p50 in enumerate((19000, 17000, 18000, 20000, 21000), start=1):
        write_w2w(work, 4, index, p50)


ENVIRONMENT = {
    "machine": {"model": "Mac17,2", "chip": "Apple M5", "memory_bytes": "34359738368"},
    "os": {"name": "macOS", "version": "27.0.1", "build": "26A434"},
    "compiler": "Apple clang version 21.0.0",
    "commit": "abc123",
    "commit_dirty": False,
    "date": "2026-10-28T12:00:00Z",
    "build_flags": "-O3 -DNDEBUG",
}


def test_parse_spsc_run_reads_every_field() -> None:
    run = phase23_report.parse_spsc_run(SPSC_TEXT, "spsc-run1.txt")
    assert run["mode"] == "batch64"
    assert run["granularity_ns"] == 41.7
    assert run["messages"] == 1000000
    assert run["accepted"] == 999999
    assert run["events_per_second"] == 1234567.0
    assert run["samples"] == 15625
    assert (run["p50_ns"], run["p90_ns"], run["p99_ns"], run["p999_ns"], run["max_ns"]) == (
        123,
        456,
        789,
        1000,
        2000,
    )
    assert run["mean_ns"] == 150.0


def test_parse_spsc_run_rejects_foreign_output() -> None:
    with pytest.raises(phase23_report.ReportError, match="spsc_latency"):
        phase23_report.parse_spsc_run("hello\n", "x.txt")


def test_median_run_is_the_upper_median() -> None:
    runs = [{"p50_ns": value} for value in (300, 100, 200, 400, 500)]
    assert phase23_report.median_run(runs)["p50_ns"] == 300


def test_build_report_keeps_every_run_and_selects_the_median(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    report = phase23_report.build_report(tmp_path, ENVIRONMENT, 0.42, 0.31)

    assert report["kind"] == "phase23_latency"
    assert report["commit"] == "abc123"
    assert report["build_flags"] == "-O3 -DNDEBUG"
    assert report["load_before_1m"] == 0.42
    assert report["timer"]["granularity_ns"] == 41.7
    assert report["timer"]["mode"] == "batch64"

    ring = report["ring"]
    assert ring["runs"] == 5
    assert ring["messages"] == 1000000
    assert ring["median"]["p50_ns"] == 300
    assert ring["p50_spread_ns"] == [100, 500]

    groups = report["wire_to_wire"]["groups"]
    assert [group["sessions"] for group in groups] == [1, 4]
    assert report["wire_to_wire"]["orders"] == 200000
    assert groups[0]["median"]["p50_ns"] == 9000
    assert groups[1]["median"]["p50_ns"] == 19000
    assert groups[0]["p50_spread_ns"] == [7000, 11000]
    assert groups[0]["runs"] == 5


def test_build_report_rejects_a_run_with_unanswered_orders(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    write_w2w(tmp_path, 1, 99, 9000, unanswered=7)
    with pytest.raises(phase23_report.ReportError, match="7 unanswered"):
        phase23_report.build_report(tmp_path, ENVIRONMENT, None, None)


def test_build_report_rejects_an_unrecovered_gap(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    write_w2w(tmp_path, 4, 99, 19000, gaps=1)
    with pytest.raises(phase23_report.ReportError, match="1 unrecovered gap"):
        phase23_report.build_report(tmp_path, ENVIRONMENT, None, None)


def test_build_report_rejects_disagreeing_ring_modes(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    write_spsc(tmp_path, 99, 300, mode="per_op")
    with pytest.raises(phase23_report.ReportError, match="sampling mode"):
        phase23_report.build_report(tmp_path, ENVIRONMENT, None, None)


def test_build_report_uses_the_median_timer_granularity(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    write_spsc(tmp_path, 1, 300, granularity="41.0")
    report = phase23_report.build_report(tmp_path, ENVIRONMENT, None, None)
    assert report["timer"]["granularity_ns"] == 41.7
    assert report["timer"]["granularity_runs_ns"] == [41.0, 41.7, 41.7, 41.7, 41.7]


def test_build_report_requires_runs(tmp_path: Path) -> None:
    with pytest.raises(phase23_report.ReportError, match="no spsc-run"):
        phase23_report.build_report(tmp_path, ENVIRONMENT, None, None)


def test_render_carries_provenance_tables_and_caveats(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    text = phase23_report.render(phase23_report.build_report(tmp_path, ENVIRONMENT, 0.5, 0.4))

    assert "# Phase 2/3 latency report" in text
    for field in (
        "| Date (UTC) |",
        "| Commit |",
        "| Machine | Mac17,2, Apple M5, 32 GiB |",
        "| OS | macOS 27.0.1 (26A434) |",
        "| Compiler | Apple clang version 21.0.0 |",
        "| Build flags | `-O3 -DNDEBUG` |",
        "| Timer | granularity 41.7 ns per step |",
        "| Ring sampling mode | `batch64` |",
        "| Load average (1 min) | 0.50 before, 0.40 after |",
    ):
        assert field in text

    assert "| p50 | 300 |" in text
    assert "| p99.9 | 1,000 |" in text
    assert "| p50 across runs | 100–500 ns |" in text
    assert "| p50 | 9,000 | 19,000 |" in text
    assert "| Requests/s (median run) | 12,346 | 12,346 |" in text
    assert "| Unrecovered market-data gaps | 0 | 0 |" in text

    assert phase23_report.RING_CAVEAT in text
    assert phase23_report.W2W_CAVEAT in text
    assert phase23_report.MACHINE_CAVEAT in text
    assert "`batch64`: every sample is the per-op average over 64 events." in text
    assert "| Orders per run | 200,000 | 200,000 |" in text
    assert "timed per request" in text
    assert "coordinated omission" in text
    assert "batch64" in text


def test_render_marks_a_dirty_tree(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    environment = dict(ENVIRONMENT, commit_dirty=True)
    text = phase23_report.render(phase23_report.build_report(tmp_path, environment, None, None))
    assert "(working tree dirty)" in text


def test_main_writes_json_and_prints_markdown(tmp_path: Path) -> None:
    fill_work_dir(tmp_path)
    destination = tmp_path / "out" / "phase23.json"
    exit_code = phase23_report.main(
        [
            "--work-dir",
            str(tmp_path),
            "--json",
            str(destination),
            "--load-before",
            "0.2",
            "--load-after",
            "0.1",
            "--build-flags",
            "-O3 -DNDEBUG",
        ]
    )
    assert exit_code == 0
    report = json.loads(destination.read_text(encoding="utf-8"))
    assert report["kind"] == "phase23_latency"
    assert report["build_flags"] == "-O3 -DNDEBUG"
