#!/usr/bin/env bash
#
# check_determinism.sh - replay the same flow with the debug and release builds
# and fail unless every engine that is available produces the same digest in
# both builds.
#
# What it does:
#   1. configures and builds the debug and release presets;
#   2. generates a synthetic flow (--orders, default 200000) with a fixed seed
#      using the release lobsim;
#   3. replays the flow with --digest for each build and each available engine.
#      The reference engine is always available; the fast engine is considered
#      available when a small probe flow yields accepts (a stub engine does not);
#   4. prints one table row per (engine, build) and checks that the debug and
#      release digests match per engine. Engine-to-engine agreement is the
#      differential test's job, not this script's, so a fast/reference mismatch
#      is reported but does not fail the build-type determinism check.
#
# Usage: scripts/check_determinism.sh [--orders N] [--seed S]
# Exit status: 0 when every check passed, 1 otherwise.
#
# Environment: VCPKG_ROOT must point at the pinned vcpkg clone; it defaults to
# $HOME/.cache/matchbook/vcpkg when that directory exists.

set -euo pipefail

ORDERS=200000
SEED=20261017

while (($# > 0)); do
  case "$1" in
    --orders) ORDERS="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "check_determinism: unknown argument '$1'" >&2
      exit 2
      ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  if [[ -d "$HOME/.cache/matchbook/vcpkg" ]]; then
    VCPKG_ROOT="$HOME/.cache/matchbook/vcpkg"
    export VCPKG_ROOT
  else
    echo "check_determinism: set VCPKG_ROOT to the pinned vcpkg clone" >&2
    exit 1
  fi
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/lob_determinism.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "matchbook-cpp determinism check"
echo "  orders: $ORDERS  seed: $SEED"
echo "  vcpkg:  $VCPKG_ROOT"
echo

for preset in debug release; do
  echo "building $preset..."
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset" >/dev/null
done
echo

RELEASE_BIN="$ROOT/build/release/lobsim"
DEBUG_BIN="$ROOT/build/debug/lobsim"

echo "generating $ORDERS-order flow..."
"$RELEASE_BIN" gen --orders "$ORDERS" --seed "$SEED" --out "$WORK/flow.bin"

# A flow small enough to reason about: two non-crossing limit submits. A real
# fast engine accepts both; the placeholder implementation accepts nothing.
cat >"$WORK/probe.csv" <<'CSV'
op,side,type,id,price,qty,ts_ns
submit,buy,limit,1,100,5,1
submit,sell,limit,2,101,5,2
CSV

summary_value() { # <summary file> <key>
  sed -n "s/^${2}: //p" "$1" | head -n 1
}

echo "engine     build     records    trades   final_orders  digest"
echo "--------------------------------------------------------------"

declare -a ENGINES=(reference)
FAST_AVAILABLE=0
"$RELEASE_BIN" replay "$WORK/probe.csv" --engine fast >"$WORK/probe_fast.out" 2>/dev/null || true
if [[ "$(summary_value "$WORK/probe_fast.out" accepts)" == "2" ]]; then
  FAST_AVAILABLE=1
  ENGINES+=(fast)
fi

FAILED=0
for engine in "${ENGINES[@]}"; do
  for preset in debug release; do
    if [[ "$preset" == "debug" ]]; then
      bin="$DEBUG_BIN"
    else
      bin="$RELEASE_BIN"
    fi
    out="$WORK/$engine.$preset.out"
    "$bin" replay "$WORK/flow.bin" --engine "$engine" --digest >"$out"
    printf '%-10s %-9s %9s %8s %13s  %s\n' \
      "$engine" "$preset" \
      "$(summary_value "$out" records)" \
      "$(summary_value "$out" trades)" \
      "$(summary_value "$out" final_orders)" \
      "$(summary_value "$out" digest)"
  done

  debug_digest="$(summary_value "$WORK/$engine.debug.out" digest)"
  release_digest="$(summary_value "$WORK/$engine.release.out" digest)"
  if [[ "$debug_digest" != "$release_digest" ]]; then
    echo "FAIL: $engine digest differs between debug ($debug_digest) and release ($release_digest)"
    FAILED=1
  fi
done

echo
if [[ "$FAST_AVAILABLE" -eq 0 ]]; then
  echo "note: fast engine did not accept the probe flow; skipped"
fi

if [[ "$FAILED" -ne 0 ]]; then
  echo "determinism: FAIL"
  exit 1
fi
echo "determinism: PASS (debug and release digests match)"
