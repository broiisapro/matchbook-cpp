#!/usr/bin/env bash
#
# check_determinism.sh - replay the same flow with the debug and release builds
# and fail unless the digests agree everywhere they must.
#
# What it does:
#   1. configures and builds the debug and release presets;
#   2. generates a synthetic flow (--orders, default 200000) with a fixed seed
#      using the release lobsim;
#   3. replays the flow with --digest for each build and each engine;
#   4. prints one table row per (engine, build) and checks two hard
#      requirements: the debug and release digests match per engine, and the
#      fast and reference engines produce the same digest within the same
#      build. Either mismatch fails the script; there is no skip path.
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
      # Print the leading comment block (from after the shebang to the first
      # blank line) as the usage text.
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
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

summary_value() { # <summary file> <key>
  sed -n "s/^${2}: //p" "$1" | head -n 1
}

echo "engine     build     records    trades   final_orders  digest"
echo "--------------------------------------------------------------"

declare -a ENGINES=(reference fast)

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

for preset in debug release; do
  fast_digest="$(summary_value "$WORK/fast.$preset.out" digest)"
  reference_digest="$(summary_value "$WORK/reference.$preset.out" digest)"
  if [[ "$fast_digest" != "$reference_digest" ]]; then
    echo "FAIL: digest differs between fast ($fast_digest) and reference ($reference_digest) in the $preset build"
    FAILED=1
  fi
done

if [[ "$FAILED" -ne 0 ]]; then
  echo "determinism: FAIL"
  exit 1
fi
echo "determinism: PASS (debug and release digests match, fast and reference agree)"
