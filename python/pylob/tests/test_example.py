"""Tests for examples/replay_spread.py."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest
from flow_builder import sample_flow, write_flow

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "examples" / "replay_spread.py"


def test_example_writes_a_spread_plot(tmp_path: Path) -> None:
    pytest.importorskip("matplotlib")
    flow = write_flow(tmp_path / "flow.bin", sample_flow(orders=20_000, seed=3))
    out = tmp_path / "nested" / "spread.png"

    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--flow",
            str(flow),
            "--out",
            str(out),
            "--every",
            "100",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    assert out.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert out.stat().st_size > 5_000
    assert "records:" in completed.stdout
    assert "samples:" in completed.stdout
    assert "spread_ticks:" in completed.stdout

    samples_line = next(
        line for line in completed.stdout.splitlines() if line.startswith("samples:")
    )
    assert int(samples_line.split(":", 1)[1]) > 100


def test_example_rejects_a_non_positive_sampling_interval(tmp_path: Path) -> None:
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--flow",
            str(tmp_path / "flow.bin"),
            "--out",
            str(tmp_path / "spread.png"),
            "--every",
            "0",
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 2
    assert "--every must be at least 1" in completed.stderr
