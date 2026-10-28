"""Tests for pylob.read_flow and pylob.replay."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest
from flow_builder import Record, sample_flow, write_csv_flow, write_flow

import pylob

SUMMARY_KEYS = {
    "records",
    "submits",
    "cancels",
    "modifies",
    "accepts",
    "rejects",
    "trades",
    "traded_qty",
    "final_orders",
    "digest",
    "wall_ms",
    "ns_per_record",
}

REPO_ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture()
def flow(tmp_path: Path) -> tuple[Path, list[Record]]:
    records = sample_flow(orders=2_000, seed=11)
    return write_flow(tmp_path / "flow.bin", records), records


def apply_records(book: pylob.OrderBook, records: list[Record]) -> None:
    for op, side, type_, id_, price, qty, _ts_ns in records:
        if op == 1:
            book.submit(id_, pylob.Side(side), pylob.OrderType(type_), price, qty)
        elif op == 2:
            book.cancel(id_)
        else:
            book.modify(id_, price, qty)


def test_read_flow_decodes_the_wire_format(flow: tuple[Path, list[Record]]) -> None:
    path, records = flow
    loaded = pylob.read_flow(str(path))

    assert len(loaded) == len(records)
    assert {record.op for record in loaded} == {"submit", "cancel"}

    first = loaded[0]
    assert (first.op, first.id, first.price, first.qty, first.ts_ns) == (
        "submit",
        records[0][3],
        records[0][4],
        records[0][5],
        records[0][6],
    )
    assert first.side == pylob.Side(records[0][1])
    assert first.type == pylob.OrderType(records[0][2])
    assert "FlowRecord(" in repr(first)


def test_replay_summary_matches_a_direct_replay(flow: tuple[Path, list[Record]]) -> None:
    path, records = flow
    summary = pylob.replay(str(path))

    assert set(summary) == SUMMARY_KEYS
    assert summary["records"] == len(records)
    assert summary["submits"] + summary["cancels"] + summary["modifies"] == len(records)
    assert summary["submits"] == sum(1 for record in records if record[0] == 1)
    assert summary["trades"] > 0
    assert summary["traded_qty"] > 0
    assert summary["wall_ms"] >= 0.0
    assert summary["ns_per_record"] >= 0.0

    book = pylob.OrderBook(pylob.Config())
    apply_records(book, records)
    assert summary["final_orders"] == book.order_count()
    assert summary["digest"] == book.digest()


def test_reference_engine_replay_matches_the_fast_engine(flow: tuple[Path, list[Record]]) -> None:
    path, _ = flow
    fast = pylob.replay(str(path), engine="fast")
    reference = pylob.replay(str(path), engine="reference")

    assert fast["digest"] == reference["digest"]
    for key in ("records", "submits", "cancels", "modifies", "accepts", "rejects", "trades"):
        assert fast[key] == reference[key]
    assert fast["final_orders"] == reference["final_orders"]


def test_replay_rejects_an_unknown_engine(flow: tuple[Path, list[Record]]) -> None:
    path, _ = flow
    with pytest.raises(ValueError):
        pylob.replay(str(path), engine="slow")


def test_replay_reports_missing_files(tmp_path: Path) -> None:
    with pytest.raises(RuntimeError):
        pylob.replay(str(tmp_path / "absent.bin"))
    with pytest.raises(RuntimeError):
        pylob.read_flow(str(tmp_path / "absent.bin"))


def test_csv_flow_replays_like_the_binary_flow(flow: tuple[Path, list[Record]], tmp_path: Path) -> None:
    binary_path, records = flow
    csv_path = write_csv_flow(tmp_path / "flow.csv", records)

    assert pylob.read_flow(str(csv_path))[0] == pylob.read_flow(str(binary_path))[0]
    assert pylob.replay(str(csv_path))["digest"] == pylob.replay(str(binary_path))["digest"]


def find_lobsim() -> Path | None:
    candidate = os.environ.get("LOBSIM")
    if candidate:
        path = Path(candidate)
        return path if path.is_file() else None
    built = REPO_ROOT / "build" / "release" / "lobsim"
    return built if built.is_file() else None


def test_digest_matches_lobsim_replay(flow: tuple[Path, list[Record]]) -> None:
    lobsim = find_lobsim()
    if lobsim is None:
        pytest.skip("lobsim is not built; run the CMake release preset to enable this check")

    path, _ = flow
    completed = subprocess.run(
        [str(lobsim), "replay", str(path), "--engine", "fast", "--digest"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr

    printed = dict(line.split(": ", 1) for line in completed.stdout.splitlines() if ": " in line)
    summary = pylob.replay(str(path))
    assert int(printed["digest"], 16) == summary["digest"]
    assert int(printed["records"]) == summary["records"]
    assert int(printed["submits"]) == summary["submits"]
    assert int(printed["trades"]) == summary["trades"]
    assert int(printed["final_orders"]) == summary["final_orders"]


def test_digest_matches_lobsim_on_a_generated_flow(tmp_path: Path) -> None:
    lobsim = find_lobsim()
    if lobsim is None:
        pytest.skip("lobsim is not built; run the CMake release preset to enable this check")

    generated = tmp_path / "generated.bin"
    completed = subprocess.run(
        [str(lobsim), "gen", "--orders", "500", "--seed", "5", "--out", str(generated)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr

    summary = pylob.replay(str(generated))
    expected = subprocess.run(
        [str(lobsim), "replay", str(generated), "--engine", "fast", "--digest"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert expected.returncode == 0, expected.stderr
    printed = dict(line.split(": ", 1) for line in expected.stdout.splitlines() if ": " in line)

    assert int(printed["digest"], 16) == summary["digest"]
    assert int(printed["records"]) == summary["records"]
