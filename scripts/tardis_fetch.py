#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Download the free first-of-month Tardis Binance ETHBTC incremental_book_L2
file for 2026-10-01 and keep the first hour of rows.

The file is public (no API key) and streamed: rows are written to the output
gzip as they arrive and the download is closed as soon as a row's timestamp is
one hour past the first row's timestamp. The result is the committed test
fixture tests/data/tardis_sample.csv.gz.

Usage: uv run scripts/tardis_fetch.py
"""

from __future__ import annotations

import gzip
import io
import os
import pathlib
import sys
import urllib.request

URL = (
    "https://datasets.tardis.dev/v1/binance/"
    "incremental_book_L2/2026/10/01/ETHBTC.csv.gz"
)
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "tardis_sample.csv.gz"
ONE_HOUR_US = 3_600 * 1_000_000
TIMESTAMP_COLUMN = "timestamp"


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    temporary = OUTPUT.with_suffix(OUTPUT.suffix + ".tmp")

    request = urllib.request.Request(URL, headers={"User-Agent": "matchbook-cpp-tardis-fetch"})
    print(f"fetching {URL}")
    with urllib.request.urlopen(request, timeout=120) as response:
        with gzip.GzipFile(fileobj=response) as decompressed:
            text = io.TextIOWrapper(decompressed, encoding="utf-8", newline="")

            header = text.readline()
            columns = header.rstrip("\r\n").split(",")
            if TIMESTAMP_COLUMN not in columns:
                print(f"error: no {TIMESTAMP_COLUMN} column in header: {header!r}", file=sys.stderr)
                return 1
            timestamp_index = columns.index(TIMESTAMP_COLUMN)

            first_timestamp: int | None = None
            rows = 0
            with gzip.open(temporary, "wt", encoding="utf-8", newline="") as output:
                output.write(header)
                for line in text:
                    fields = line.rstrip("\r\n").split(",")
                    if len(fields) <= timestamp_index:
                        print(f"error: short row after {rows} rows: {line!r}", file=sys.stderr)
                        return 1
                    timestamp = int(fields[timestamp_index])
                    if first_timestamp is None:
                        first_timestamp = timestamp
                    if timestamp - first_timestamp >= ONE_HOUR_US:
                        break
                    output.write(line)
                    rows += 1

    os.replace(temporary, OUTPUT)
    size = OUTPUT.stat().st_size
    print(f"wrote {OUTPUT} ({rows} rows, {size / 1_000_000:.2f} MB gzipped)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
