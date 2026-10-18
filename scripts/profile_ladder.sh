#!/usr/bin/env bash
#
# Profile where each price ladder spends its time.
#
#   scripts/profile_ladder.sh [--min-time S] [--fallback] [--no-fallback]
#                              [--no-build]
#
# Steps:
#   1. bootstrap the pinned vcpkg (VCPKG_ROOT, default ~/.cache/matchbook/vcpkg),
#   2. configure `release` into build/profile-array (the array ladder, the
#      post-#58 default, pinned explicitly so the pair always means array vs
#      tree) and `release-tree` into build/profile-tree, both with `-g` added
#      via CMAKE_CXX_FLAGS, and build the lob_bench target in each,
#   3. record a `xcrun xctrace record --template 'Time Profiler'` trace around
#      `lob_bench --benchmark_filter=MixedFlow` for each build (the trace and
#      the exported samples stay next to the binary under build/),
#   4. export the time-profile table and print the top 15 symbols by self time
#      for the tree and the array build side by side.
#
# Self time is estimated from Time Profiler samples: one sample is one
# millisecond of on-CPU time. The trace covers the whole lob_bench process, so
# the one-time prologue and Google Benchmark's own setup are included in the
# shares; the MixedFlow loop dominates when --min-time is at least a few
# seconds.
#
# No root is needed. macOS may show a one-time "Developer Tools Access"
# permission dialog the first time a session records with xctrace; accept it
# once and later runs are non-interactive. In sessions without a window server
# (headless shells, CI) xctrace can refuse to start: the script then says so
# and falls back to comparing the lob_bench JSON counters (p50/p90/p99/p99.9/
# mean and throughput) for the same benchmark on both builds. Pass --fallback
# to skip xctrace entirely, or --no-fallback to fail instead of falling back.
#
# These are profiling numbers, not publishable benchmark numbers:
# scripts/run_benchmarks.sh remains the only sanctioned producer of those.
#
# Environment:
#   VCPKG_ROOT             vcpkg checkout to use (default ~/.cache/matchbook/vcpkg)
#   LOB_PROFILE_MIN_TIME   lob_bench --benchmark_min_time (default 5s)

set -euo pipefail

min_time="${LOB_PROFILE_MIN_TIME:-5s}"
benchmark_filter="MixedFlow"
use_fallback=0
no_fallback=0
do_build=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --min-time)
      if [[ $# -lt 2 ]]; then
        echo "profile_ladder: --min-time needs a value (for example 10s)" >&2
        exit 2
      fi
      min_time="$2"
      shift 2
      ;;
    --min-time=*)
      min_time="${1#--min-time=}"
      shift
      ;;
    --fallback)
      use_fallback=1
      shift
      ;;
    --no-fallback)
      no_fallback=1
      shift
      ;;
    --no-build)
      do_build=0
      shift
      ;;
    -h|--help)
      sed -n '2,38p' "$0"
      exit 0
      ;;
    *)
      echo "profile_ladder: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
cd "$repo_root"

export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/matchbook/vcpkg}"

array_dir="$repo_root/build/profile-array"
tree_dir="$repo_root/build/profile-tree"

log() {
  printf 'profile_ladder: %s\n' "$*"
}

build_profile() {
  local preset="$1"
  local dir="$2"
  shift 2
  log "configuring $preset into $dir (CMAKE_CXX_FLAGS=-g)"
  cmake --preset "$preset" -B "$dir" -DCMAKE_CXX_FLAGS="-g" "$@" >/dev/null
  log "building lob_bench in $dir"
  cmake --build "$dir" --target lob_bench
}

# Records one trace and exports the time-profile samples. Returns non-zero
# when xctrace cannot run; the caller decides whether to fall back.
record_trace() {
  local dir="$1"
  local label="$2"
  local trace="$dir/mixedflow.trace"
  local samples="$dir/mixedflow-samples.xml"
  rm -rf "$trace" "$samples"

  log "recording $label with the Time Profiler template (no root needed;"
  log "  accept the one-time Developer Tools / Instruments dialog if macOS shows one)"
  if ! (
    cd "$dir" &&
      xcrun xctrace record --template 'Time Profiler' --output "$trace" --launch -- \
        ./lob_bench "--benchmark_filter=$benchmark_filter" "--benchmark_min_time=$min_time"
  ); then
    log "xctrace record failed for $label"
    return 1
  fi
  log "exporting the time-profile samples to $samples (this can take a minute)"
  if ! xcrun xctrace export --input "$trace" \
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' >"$samples"; then
    log "xctrace export failed for $label"
    return 1
  fi
  return 0
}

# Prints the top 15 self-time symbols of the tree and array samples side by
# side. One Time Profiler sample is one millisecond of on-CPU time; the leaf
# frame of each sample owns its self time.
report_side_by_side() {
  local tree_samples="$1"
  local array_samples="$2"
  python3 - "$tree_samples" "$array_samples" <<'PY'
import sys
import xml.etree.ElementTree as ET
from collections import Counter

def resolve(element, by_id):
    hops = 0
    while element is not None and element.get('ref') and hops < 16:
        element = by_id.get(element.get('ref'))
        hops += 1
    return element

def self_samples(path):
    root = ET.parse(path).getroot()
    by_id = {}
    for element in root.iter():
        if element.get('id'):
            by_id[element.get('id')] = element
    counts = Counter()
    total = 0
    for row in root.iter('row'):
        total += 1
        backtrace = resolve(row.find('tagged-backtrace'), by_id)
        leaf = None
        if backtrace is not None:
            for frame in backtrace.findall('frame'):
                frame = resolve(frame, by_id)
                if frame is not None and frame.get('name'):
                    leaf = frame.get('name')
                    break
        counts[leaf or '(no frames)'] += 1
    return counts, total

def shorten(symbol, width=72):
    return symbol if len(symbol) <= width else symbol[: width - 3] + '...'

tree_counts, tree_total = self_samples(sys.argv[1])
array_counts, array_total = self_samples(sys.argv[2])
tree_rows = tree_counts.most_common(15)
array_rows = array_counts.most_common(15)

print()
print(f'self time from {tree_total} / {array_total} samples '
      f'(1 sample = 1 ms on-CPU; prologue and harness setup included)')
print(f'{"rank":>4}  {"tree ladder":<74}{"self%":>7}  |  {"array ladder":<74}{"self%":>7}')
for rank in range(15):
    left = tree_rows[rank] if rank < len(tree_rows) else (None, 0)
    right = array_rows[rank] if rank < len(array_rows) else (None, 0)
    left_name = shorten(left[0]) if left[0] is not None else ''
    right_name = shorten(right[0]) if right[0] is not None else ''
    left_pct = 100.0 * left[1] / tree_total if tree_total else 0.0
    right_pct = 100.0 * right[1] / array_total if array_total else 0.0
    print(f'{rank + 1:>4}  {left_name:<74}{left_pct:>6.2f}%  |  {right_name:<74}{right_pct:>6.2f}%')
PY
}

# Fallback when xctrace cannot run: run the same benchmark on both builds and
# compare the counters lob_bench writes to bench.json.
report_counters() {
  log "running lob_bench $benchmark_filter on both builds for the counter comparison"
  (cd "$array_dir" && ./lob_bench "--benchmark_filter=$benchmark_filter" \
      "--benchmark_min_time=$min_time" >/dev/null 2>&1)
  (cd "$tree_dir" && ./lob_bench "--benchmark_filter=$benchmark_filter" \
      "--benchmark_min_time=$min_time" >/dev/null 2>&1)
  python3 - "$tree_dir/bench.json" "$array_dir/bench.json" <<'PY'
import json
import sys

def mixed(path):
    with open(path) as handle:
        data = json.load(handle)
    for benchmark in data['benchmarks']:
        if benchmark['name'] == 'MixedFlow':
            return benchmark
    raise SystemExit(f"no MixedFlow benchmark in {path}")

tree = mixed(sys.argv[1])
array = mixed(sys.argv[2])
print()
print('MixedFlow counters (lob_bench JSON; no trace was taken)')
print(f'{"counter":<18}{"tree ladder":>14}{"array ladder":>14}{"array/tree":>12}')
for key in ('p50_ns', 'p90_ns', 'p99_ns', 'p999_ns', 'mean_ns'):
    left = tree.get(key)
    right = array.get(key)
    ratio = f'{right / left:.3f}' if left else '-'
    print(f'{key:<18}{left:>14.1f}{right:>14.1f}{ratio:>12}')
left = tree.get('items_per_second')
right = array.get('items_per_second')
ratio = f'{right / left:.3f}' if left else '-'
print(f'{"ops_per_second":<18}{left:>14.0f}{right:>14.0f}{ratio:>12}')
PY
}

if [[ "$do_build" == 1 ]]; then
  ./scripts/bootstrap_vcpkg.sh
  build_profile release "$array_dir" -DLOB_PRICE_LADDER=array
  build_profile release-tree "$tree_dir"
fi

if [[ "$use_fallback" == 1 ]]; then
  report_counters
  exit 0
fi

if record_trace "$array_dir" "the array ladder" && record_trace "$tree_dir" "the tree ladder"; then
  report_side_by_side "$tree_dir/mixedflow-samples.xml" "$array_dir/mixedflow-samples.xml"
else
  if [[ "$no_fallback" == 1 ]]; then
    log "xctrace did not run; --no-fallback was passed"
    exit 1
  fi
  log "xctrace could not record non-interactively here (permissions or no window server);"
  log "falling back to the lob_bench counter comparison instead; no trace output is faked"
  report_counters
fi
