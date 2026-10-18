#!/usr/bin/env bash
#
# Produce publishable benchmark numbers on an idle machine.
#
#   scripts/run_benchmarks.sh [--compare] [--force] [--dry-run]
#
# Single-build mode (default):
#   1. bootstraps the pinned vcpkg (VCPKG_ROOT, default ~/.cache/matchbook/vcpkg),
#   2. configures and builds the release preset,
#   3. runs `lob_bench --self-test` and stops if the timing harness is broken,
#   4. runs the DoD benchmark command with five repetitions
#      (`AddOrder|Cancel|Market|MixedFlow`, the DoD filter from decision C31,
#      so the report has the throughput row),
#   5. renders `build/release/bench.md` from the JSON with bench_report.py.
#
# Compare mode (--compare): builds the tree baseline and the array build,
# alternates them over two benchmark rounds (tree/array then array/tree) so
# thermal drift hits both sides, merges each build's rounds into
# `build/<preset>/bench.json` and renders `build/bench-compare.md`.
#
# It refuses to start (unless --force) while the machine is busy: the
# 1-minute load average must be at or below LOB_BENCH_MAX_LOAD (default 1.0)
# and no cmake, ninja, clang or GitHub Actions runner worker processes may be
# running. Everything it found is printed. --dry-run prints the commands and
# exits without touching the machine.
#
# Environment:
#   VCPKG_ROOT          vcpkg checkout to use (default ~/.cache/matchbook/vcpkg)
#   LOB_BENCH_MAX_LOAD  load-average threshold (default 1.0)
#   LOB_BENCH_REPETITIONS  repetitions per benchmark invocation (default 5;
#                          compare mode runs two invocations per build)
#   LOB_BENCH_BEFORE_PRESET  compare-mode before preset (default release-tree)
#   LOB_BENCH_AFTER_PRESET   compare-mode after preset (default release, the
#                            array ladder)

set -euo pipefail

compare=0
force=0
dry_run=0
for argument in "$@"; do
  case "$argument" in
    --compare) compare=1 ;;
    --force) force=1 ;;
    --dry-run) dry_run=1 ;;
    -h|--help)
      sed -n '2,39p' "$0"
      exit 0
      ;;
    *)
      echo "run_benchmarks: unknown argument: $argument" >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
cd "$repo_root"

export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/matchbook/vcpkg}"
load_threshold="${LOB_BENCH_MAX_LOAD:-1.0}"
repetitions="${LOB_BENCH_REPETITIONS:-5}"
benchmark_filter='AddOrder|Cancel|Market|MixedFlow'

load_average_1m() {
  sysctl -n vm.loadavg | awk '{print $2}'
}

# Prints everything that makes the machine busy. Returns 0 when something was
# found, 1 when the machine looks idle.
noise_report() {
  local found=1
  local load
  load="$(load_average_1m)"
  if awk -v load="$load" -v threshold="$load_threshold" 'BEGIN { exit !(load > threshold) }'; then
    echo "run_benchmarks: 1-minute load average ${load} is above ${load_threshold}"
    found=0
  fi
  local process
  for process in cmake ninja clang; do
    if pgrep -x "$process" >/dev/null 2>&1; then
      echo "run_benchmarks: active ${process} processes:"
      pgrep -lx "$process" | sed 's/^/  /'
      found=0
    fi
  done
  if pgrep -f 'Runner.Worker' >/dev/null 2>&1; then
    echo "run_benchmarks: active GitHub Actions runner workers:"
    pgrep -fl 'Runner.Worker' | sed 's/^/  /'
    found=0
  fi
  return "$found"
}

check_idle_or_stop() {
  if [[ "$force" == 0 ]]; then
    if noise_report; then
      echo "run_benchmarks: refusing to run on a busy machine; rerun on an idle machine or pass --force" >&2
      exit 1
    fi
  else
    echo "run_benchmarks: --force, skipping the noise guard; numbers may be noise" >&2
  fi
}

run() {
  echo "+ $*"
  if [[ "$dry_run" == 0 ]]; then
    "$@"
  fi
}

bench_command() {
  # $1 binary, $2 output json
  run "$1" "--benchmark_filter=${benchmark_filter}" \
      "--benchmark_repetitions=${repetitions}" \
      "--benchmark_out=$2" "--benchmark_out_format=json"
}

# ---------------------------------------------------------------------------
# Single-build mode
# ---------------------------------------------------------------------------
if [[ "$compare" == 0 ]]; then
  build_dir="$repo_root/build/release"
  bench_binary="$build_dir/lob_bench"
  bench_json="$build_dir/bench.json"
  bench_markdown="$build_dir/bench.md"

  echo "run_benchmarks: release benchmark run"
  echo "run_benchmarks: build   $build_dir"
  echo "run_benchmarks: report  $bench_markdown"
  echo "run_benchmarks: load threshold ${load_threshold}, repetitions ${repetitions}"

  if [[ "$dry_run" == 1 ]]; then
    run ./scripts/bootstrap_vcpkg.sh
    run cmake --preset release
    run cmake --build --preset release
    run "$bench_binary" --self-test
    bench_command "$bench_binary" "$bench_json"
    run uv run scripts/bench_report.py "$bench_json"
    echo "run_benchmarks: dry run, nothing was executed"
    exit 0
  fi

  check_idle_or_stop

  run ./scripts/bootstrap_vcpkg.sh
  run cmake --preset release
  run cmake --build --preset release
  run "$bench_binary" --self-test
  bench_command "$bench_binary" "$bench_json"
  if [[ ! -f "$bench_json" ]]; then
    echo "run_benchmarks: expected $bench_json was not written" >&2
    exit 1
  fi
  if ! uv run scripts/bench_report.py "$bench_json" >"$bench_markdown"; then
    echo "run_benchmarks: report rendering failed" >&2
    exit 1
  fi
  echo "run_benchmarks: wrote $bench_markdown"
  exit 0
fi

# ---------------------------------------------------------------------------
# Compare mode: tree baseline vs array build, alternating rounds
# ---------------------------------------------------------------------------
before_preset="${LOB_BENCH_BEFORE_PRESET:-release-tree}"
after_preset="${LOB_BENCH_AFTER_PRESET:-release}"
before_dir="$repo_root/build/$before_preset"
after_dir="$repo_root/build/$after_preset"
before_binary="$before_dir/lob_bench"
after_binary="$after_dir/lob_bench"
comparison_markdown="$repo_root/build/bench-compare.md"
rounds=2

echo "run_benchmarks: compare run (${before_preset} vs ${after_preset})"
echo "run_benchmarks: before  $before_dir/bench.json"
echo "run_benchmarks: after   $after_dir/bench.json"
echo "run_benchmarks: report  $comparison_markdown"
echo "run_benchmarks: load threshold ${load_threshold}, repetitions ${repetitions} per round, ${rounds} rounds"

if [[ "$dry_run" == 1 ]]; then
  run ./scripts/bootstrap_vcpkg.sh
  run cmake --preset "$before_preset"
  run cmake --build --preset "$before_preset"
  run "$before_binary" --self-test
  run cmake --preset "$after_preset"
  run cmake --build --preset "$after_preset"
  run "$after_binary" --self-test
  for round in $(seq 0 $((rounds - 1))); do
    if (( round % 2 == 0 )); then
      bench_command "$before_binary" "$before_dir/bench-round$round.json"
      bench_command "$after_binary" "$after_dir/bench-round$round.json"
    else
      bench_command "$after_binary" "$after_dir/bench-round$round.json"
      bench_command "$before_binary" "$before_dir/bench-round$round.json"
    fi
  done
  run uv run scripts/bench_report.py --merge "$before_dir/bench.json" \
      "$before_dir/bench-round0.json" "$before_dir/bench-round1.json"
  run uv run scripts/bench_report.py --merge "$after_dir/bench.json" \
      "$after_dir/bench-round0.json" "$after_dir/bench-round1.json"
  run uv run scripts/bench_report.py --before "$before_dir/bench.json" \
      --after "$after_dir/bench.json"
  echo "run_benchmarks: dry run, nothing was executed"
  exit 0
fi

check_idle_or_stop

run ./scripts/bootstrap_vcpkg.sh
run cmake --preset "$before_preset"
run cmake --build --preset "$before_preset"
run "$before_binary" --self-test
run cmake --preset "$after_preset"
run cmake --build --preset "$after_preset"
run "$after_binary" --self-test

for round in $(seq 0 $((rounds - 1))); do
  if (( round % 2 == 0 )); then
    bench_command "$before_binary" "$before_dir/bench-round$round.json"
    bench_command "$after_binary" "$after_dir/bench-round$round.json"
  else
    bench_command "$after_binary" "$after_dir/bench-round$round.json"
    bench_command "$before_binary" "$before_dir/bench-round$round.json"
  fi
done

for preset_dir in "$before_dir" "$after_dir"; do
  for round in $(seq 0 $((rounds - 1))); do
    if [[ ! -f "$preset_dir/bench-round$round.json" ]]; then
      echo "run_benchmarks: expected $preset_dir/bench-round$round.json was not written" >&2
      exit 1
    fi
  done
  run uv run scripts/bench_report.py --merge "$preset_dir/bench.json" \
      "$preset_dir/bench-round0.json" "$preset_dir/bench-round1.json"
  rm -f "$preset_dir/bench-round0.json" "$preset_dir/bench-round1.json"
done

if ! uv run scripts/bench_report.py --before "$before_dir/bench.json" \
    --after "$after_dir/bench.json" >"$comparison_markdown"; then
  echo "run_benchmarks: comparison rendering failed" >&2
  exit 1
fi

ladder_of() {
  grep -o '"price_ladder": *"[a-z]*"' "$1" 2>/dev/null |
    head -n 1 | sed -E 's/.*"([a-z]+)"$/\1/' || true
}

before_ladder="$(ladder_of "$before_dir/bench.json")"
after_ladder="$(ladder_of "$after_dir/bench.json")"
echo "run_benchmarks: before ladder ${before_ladder:-unknown}; after ladder ${after_ladder:-unknown}"
if [[ -n "$before_ladder" && "$before_ladder" == "$after_ladder" ]]; then
  echo "run_benchmarks: warning: both sides use the '${before_ladder}' ladder; the comparison expects" \
       "${before_preset} to be the tree baseline and ${after_preset} to be the array ladder" >&2
fi

echo "run_benchmarks: wrote $comparison_markdown"
cat "$comparison_markdown"
