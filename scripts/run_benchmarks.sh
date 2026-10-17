#!/usr/bin/env bash
#
# Produce publishable benchmark numbers on an idle machine.
#
#   scripts/run_benchmarks.sh [--force] [--dry-run]
#
# The script:
#   1. bootstraps the pinned vcpkg (VCPKG_ROOT, default ~/.cache/matchbook/vcpkg),
#   2. configures and builds the release preset,
#   3. runs `lob_bench --self-test` and stops if the timing harness is broken,
#   4. runs the DoD benchmark command with five repetitions
#      (`AddOrder|Cancel|Market` plus MixedFlow, so the report has the
#      throughput row),
#   5. renders `build/release/bench.md` from the JSON with bench_report.py.
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
#   LOB_BENCH_REPETITIONS  repetitions passed to Google Benchmark (default 5)

set -euo pipefail

force=0
dry_run=0
for argument in "$@"; do
  case "$argument" in
    --force) force=1 ;;
    --dry-run) dry_run=1 ;;
    -h|--help)
      sed -n '2,25p' "$0"
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

build_dir="$repo_root/build/release"
bench_binary="$build_dir/lob_bench"
bench_json="$build_dir/bench.json"
bench_markdown="$build_dir/bench.md"

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

run() {
  echo "+ $*"
  if [[ "$dry_run" == 0 ]]; then
    "$@"
  fi
}

echo "run_benchmarks: release benchmark run"
echo "run_benchmarks: build   $build_dir"
echo "run_benchmarks: report  $bench_markdown"
echo "run_benchmarks: load threshold ${load_threshold}, repetitions ${repetitions}"

if [[ "$dry_run" == 1 ]]; then
  run ./scripts/bootstrap_vcpkg.sh
  run cmake --preset release
  run cmake --build --preset release
  run "$bench_binary" --self-test
  run "$bench_binary" "--benchmark_filter=${benchmark_filter}" \
      "--benchmark_repetitions=${repetitions}"
  run uv run scripts/bench_report.py "$bench_json"
  echo "run_benchmarks: dry run, nothing was executed"
  exit 0
fi

if [[ "$force" == 0 ]]; then
  if noise_report; then
    echo "run_benchmarks: refusing to run on a busy machine; rerun on an idle machine or pass --force" >&2
    exit 1
  fi
else
  echo "run_benchmarks: --force, skipping the noise guard; numbers may be noise" >&2
fi

run ./scripts/bootstrap_vcpkg.sh
run cmake --preset release
run cmake --build --preset release
run "$bench_binary" --self-test
run "$bench_binary" "--benchmark_filter=${benchmark_filter}" \
    "--benchmark_repetitions=${repetitions}"
if [[ ! -f "$bench_json" ]]; then
  echo "run_benchmarks: expected $bench_json was not written" >&2
  exit 1
fi
if ! uv run scripts/bench_report.py "$bench_json" >"$bench_markdown"; then
  echo "run_benchmarks: report rendering failed" >&2
  exit 1
fi
echo "run_benchmarks: wrote $bench_markdown"
