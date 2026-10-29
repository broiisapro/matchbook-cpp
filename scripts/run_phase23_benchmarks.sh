#!/usr/bin/env bash
#
# Produce the publishable Phase 2/3 latency numbers on an idle Mac
# (decision C59, issue #154).
#
#   scripts/run_phase23_benchmarks.sh [--force] [--dry-run]
#
# What it runs, all from the release build:
#   1. `spsc_latency` (ring enqueue to `on_accept`) five times; the medians
#      across those repetitions are reported,
#   2. native closed-loop wire-to-wire runs (gateway + publisher + load
#      generator on loopback) with 1 session and with 4 sessions, five
#      repetitions each, 200,000 orders per run.
#
# It writes `build/release/phase23.json` (every repetition plus the median and
# the p50 spread) and renders `build/release/phase23.md` with the machine, OS,
# compiler, flags, timer mode and granularity, load before and after, commit,
# date, the percentile tables and the caveats that keep the numbers honest.
#
# It refuses to start (unless --force) while the machine is busy: the 1-minute
# load average must be at or below LOB_BENCH_MAX_LOAD (default 1.0) and no
# cmake, ninja, clang, GitHub Actions runner worker or Docker build may be
# running. Everything it found is printed. After the build it waits (at most
# three minutes) for the load average the build raised to fall back under the
# threshold before the first measurement. --dry-run prints the commands and
# exits without touching the machine.
#
# Environment:
#   VCPKG_ROOT             vcpkg checkout to use (default ~/.cache/matchbook/vcpkg)
#   LOB_BENCH_MAX_LOAD     load-average threshold (default 1.0)
#   PHASE23_REPETITIONS    repetitions of every series (default 5; published
#                          numbers need at least 5)
#   PHASE23_ORDERS         orders per wire-to-wire run (default 200000)
#   PHASE23_MESSAGES       ring events per run (default 1000000)

set -uo pipefail

force=0
dry_run=0
for argument in "$@"; do
  case "$argument" in
    --force) force=1 ;;
    --dry-run) dry_run=1 ;;
    -h | --help)
      sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "run_phase23_benchmarks: unknown argument: $argument" >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
cd "$repo_root"

export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/matchbook/vcpkg}"
load_threshold="${LOB_BENCH_MAX_LOAD:-1.0}"
repetitions="${PHASE23_REPETITIONS:-5}"
orders="${PHASE23_ORDERS:-200000}"
messages="${PHASE23_MESSAGES:-1000000}"

for value in "$repetitions" "$orders" "$messages"; do
  if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
    echo "run_phase23_benchmarks: repetitions, orders and messages must be positive integers" >&2
    exit 2
  fi
done
if (( repetitions < 5 || orders < 200000 )); then
  echo "run_phase23_benchmarks: warning: fewer than five repetitions or fewer than" \
       "200,000 orders; the numbers are a smoke test, not publishable" >&2
fi

build_dir="$repo_root/build/release"
work_dir="$build_dir/phase23-work"
report_json="$build_dir/phase23.json"
report_markdown="$build_dir/phase23.md"
ring_binary="$build_dir/spsc_latency"
gateway_binary="$build_dir/apps/gateway/lob_gateway"
publisher_binary="$build_dir/apps/publisher/lob_publisher"
loadgen_binary="$build_dir/apps/loadgen/lob_loadgen"

log() { printf 'run_phase23_benchmarks: %s\n' "$*"; }

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "run_phase23_benchmarks: the ring harness uses the macOS timer; run this on the Mac" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "run_phase23_benchmarks: python3 is required (loopback ports and report checks)" >&2
  exit 2
fi

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
    echo "run_phase23_benchmarks: 1-minute load average ${load} is above ${load_threshold}"
    found=0
  fi
  local process
  for process in cmake ninja clang; do
    if pgrep -x "$process" >/dev/null 2>&1; then
      echo "run_phase23_benchmarks: active ${process} processes:"
      pgrep -lx "$process" | sed 's/^/  /'
      found=0
    fi
  done
  if pgrep -f 'Runner.Worker' >/dev/null 2>&1; then
    echo "run_phase23_benchmarks: active GitHub Actions runner workers:"
    pgrep -fl 'Runner.Worker' | sed 's/^/  /'
    found=0
  fi
  if pgrep -f 'docker(-compose)? .*build|buildkitd' >/dev/null 2>&1; then
    echo "run_phase23_benchmarks: active Docker builds:"
    pgrep -fl 'docker(-compose)? .*build|buildkitd' | sed 's/^/  /'
    found=0
  fi
  return "$found"
}

check_idle_or_stop() {
  if [[ "$force" == 0 ]]; then
    if noise_report; then
      echo "run_phase23_benchmarks: refusing to run on a busy machine; rerun on an idle machine or pass --force" >&2
      exit 1
    fi
  else
    echo "run_phase23_benchmarks: --force, skipping the noise guard; numbers may be noise" >&2
  fi
}

# The release build itself raises the 1-minute load average, which would then
# be baked into the measured runs and into the provenance row. Wait (bounded)
# for it to fall back under the threshold before the first measurement.
wait_for_idle() {
  local waited=0
  local load
  while true; do
    load="$(load_average_1m)"
    if ! awk -v load="$load" -v threshold="$load_threshold" \
      'BEGIN { exit !(load > threshold) }'; then
      return 0
    fi
    if ((waited >= 180)); then
      return 1
    fi
    log "waiting for the load average ${load} to fall to ${load_threshold} (${waited}s)"
    sleep 5
    waited=$((waited + 5))
  done
}

# ---------------------------------------------------------------------------
# Process plumbing for one wire-to-wire run. Every process this script starts
# is recorded by PID and stopped on every exit path; nothing is ever found by
# name.
# ---------------------------------------------------------------------------
children=()
started_pid=0

start() {
  local log_path="$1"
  shift
  "$@" >"$log_path" 2>&1 &
  started_pid=$!
  children+=("$started_pid")
}

forget_pid() {
  local keep=()
  local pid
  for pid in "${children[@]}"; do
    [[ -z "$pid" || "$pid" == "$1" ]] || keep+=("$pid")
  done
  children=("${keep[@]}")
}

stop_children() {
  local pid
  (( ${#children[@]} == 0 )) && return 0
  for pid in "${children[@]}"; do
    [[ -n "$pid" ]] || continue
    kill -TERM "$pid" 2>/dev/null || true
  done
  local waited
  for pid in "${children[@]}"; do
    [[ -n "$pid" ]] || continue
    for ((waited = 0; waited < 100; ++waited)); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.02
    done
  done
  for pid in "${children[@]}"; do
    [[ -n "$pid" ]] || continue
    kill -KILL "$pid" 2>/dev/null || true
  done
  wait "${children[@]}" 2>/dev/null || true
  children=()
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM HUP
  stop_children
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 129' HUP
trap 'exit 143' TERM

process_running() {
  local state
  state="$(ps -o state= -p "$1" 2>/dev/null)"
  [[ -n "$state" && "$state" != Z* ]]
}

free_ports() {
  python3 - "$1" <<'PY'
import socket
import sys

count = int(sys.argv[1])
sockets = []
try:
    for _ in range(count):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
}

tcp_open() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

wait_until() {
  local pid="$1"
  local description="$2"
  shift 2
  local deadline=$((SECONDS + 20))
  while true; do
    if ! process_running "$pid"; then
      log "${description}: process exited before it was ready"
      return 1
    fi
    if "$@"; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      log "${description}: not ready after 20s"
      return 1
    fi
    sleep 0.02
  done
}

# check_w2w_report PATH SESSIONS ORDERS: rejects a run that would make the
# numbers dishonest, and prints its one-line summary.
check_w2w_report() {
  python3 - "$1" "$2" "$3" <<'PY'
import json
import sys

with open(sys.argv[1]) as handle:
    report = json.load(handle)

sessions = int(sys.argv[2])
orders = int(sys.argv[3])
problems = []
if report.get("sessions") != sessions:
    problems.append(f"ran {report.get('sessions')} session(s), expected {sessions}")
if report.get("orders_requested") != orders:
    problems.append(f"sent {report.get('orders_requested')} orders, expected {orders}")
if report.get("orders_without_response") != 0:
    problems.append(f"{report.get('orders_without_response')} order(s) without a response")
if report.get("timed_out"):
    problems.append("the run timed out")
md = report.get("md")
if not isinstance(md, dict) or md.get("received", 0) <= 0:
    problems.append("no market-data frames were received")
else:
    gap_fields = [key for key in ("sequence_gaps_unrecovered", "unrecovered_gaps") if key in md]
    if not gap_fields:
        problems.append("the market-data block has no unrecovered-gap counter")
    for key in gap_fields:
        if md[key] != 0:
            problems.append(f"{md[key]} unrecovered market-data gap(s) ({key})")
if problems:
    sys.exit("run_phase23_benchmarks: " + "; ".join(problems))

latency = report["latency_ns"]
print(
    f"  {sessions} session(s): p50 {latency['p50_ns']:,} ns, "
    f"p99 {latency['p99_ns']:,} ns, "
    f"{report['requests_per_second']:,.0f} requests/s, "
    f"{md['received']:,} market-data frames, 0 gaps"
)
PY
}

# wire_once SESSIONS REPORT_PATH LABEL: one gateway + publisher + load
# generator run on loopback with fresh ports. Returns non-zero when the run
# cannot be used.
wire_once() {
  local sessions="$1"
  local report_path="$2"
  local label="$3"
  local gateway_port md_port recovery_port loadgen_udp
  read -r gateway_port md_port recovery_port loadgen_udp < <(free_ports 4) || return 1
  if [[ -z "$loadgen_udp" ]]; then
    log "could not allocate loopback ports"
    return 1
  fi

  start "$work_dir/gateway-${label}.log" "$gateway_binary" \
    --port "$gateway_port" --md-port "$md_port"
  local gateway_pid=$started_pid
  wait_until "$gateway_pid" "gateway" tcp_open "$gateway_port" || return 1
  wait_until "$gateway_pid" "gateway md stream" tcp_open "$md_port" || return 1

  start "$work_dir/publisher-${label}.log" "$publisher_binary" \
    --gateway "127.0.0.1:$md_port" \
    --udp-dest "127.0.0.1:$loadgen_udp" \
    --recovery-port "$recovery_port"
  local publisher_pid=$started_pid
  wait_until "$publisher_pid" "publisher" tcp_open "$recovery_port" || return 1
  wait_until "$publisher_pid" "publisher gateway connection" \
    grep -q "connected to gateway" "$work_dir/publisher-${label}.log" || return 1

  local loadgen_pid loadgen_status=0
  "$loadgen_binary" \
    --gateway "127.0.0.1:$gateway_port" \
    --orders "$orders" \
    --sessions "$sessions" \
    --report "$report_path" \
    --md-udp-port "$loadgen_udp" \
    --recovery "127.0.0.1:$recovery_port" \
    >"$work_dir/loadgen-${label}.log" 2>&1 &
  loadgen_pid=$!
  children+=("$loadgen_pid")
  wait "$loadgen_pid" || loadgen_status=$?
  forget_pid "$loadgen_pid"
  if (( loadgen_status != 0 )); then
    log "load generator failed (${label}); see $work_dir/loadgen-${label}.log"
    cat "$work_dir/loadgen-${label}.log" >&2
    return 1
  fi

  local name pid
  for name in gateway publisher; do
    case "$name" in
      gateway) pid=$gateway_pid ;;
      publisher) pid=$publisher_pid ;;
    esac
    if ! process_running "$pid"; then
      log "${name} exited during ${label}"
      return 1
    fi
  done

  stop_children
  if ! check_w2w_report "$report_path" "$sessions" "$orders"; then
    return 1
  fi
  return 0
}

# run_wire_to_wire SESSIONS REPORT_PATH LABEL: wire_once with up to three
# attempts when a chosen port was taken between allocation and bind. A real
# failure (a bad report, a crash) is returned immediately.
run_wire_to_wire() {
  local sessions="$1"
  local report_path="$2"
  local label="$3"
  local attempt status
  for ((attempt = 1; attempt <= 3; ++attempt)); do
    wire_once "$sessions" "$report_path" "$label"
    status=$?
    if (( status == 0 )); then
      return 0
    fi
    stop_children
    if (( attempt < 3 )) &&
      grep -qi "address already in use" "$work_dir"/*-"$label".log 2>/dev/null; then
      log "a port for ${label} was taken; retrying with fresh ports"
      continue
    fi
    return "$status"
  done
  return 1
}

# ---------------------------------------------------------------------------
# Dry run: print every command, touch nothing.
# ---------------------------------------------------------------------------
print_wire_to_wire_plan() {
  local sessions="$1"
  local label="$2"
  echo "+ lob_gateway --port <ephemeral> --md-port <ephemeral>"
  echo "+ lob_publisher --gateway 127.0.0.1:<md-port> --udp-dest 127.0.0.1:<loadgen-md-port> --recovery-port <ephemeral>"
  echo "+ lob_loadgen --gateway 127.0.0.1:<entry-port> --orders ${orders} --sessions ${sessions}" \
       "--report ${work_dir}/w2w-${label}.json --md-udp-port <loadgen-md-port> --recovery 127.0.0.1:<recovery-port>"
}

if [[ "$dry_run" == 1 ]]; then
  echo "run_phase23_benchmarks: release build and report"
  echo "run_phase23_benchmarks: work    $work_dir"
  echo "run_phase23_benchmarks: report  $report_markdown"
  echo "run_phase23_benchmarks: load threshold ${load_threshold}, ${repetitions} repetitions, ${orders} orders per wire-to-wire run"
  echo "+ ./scripts/bootstrap_vcpkg.sh"
  echo "+ cmake --preset release"
  echo "+ cmake --build --preset release --target lob_gateway lob_publisher lob_loadgen spsc_latency"
  for ((run = 1; run <= repetitions; ++run)); do
    if (( run == 1 )); then
      echo "+ ${ring_binary} --messages ${messages} > ${work_dir}/spsc-run${run}.txt"
    else
      echo "+ ${ring_binary} --messages ${messages} --latency-mode <mode chosen by run 1>" \
           "> ${work_dir}/spsc-run${run}.txt"
    fi
  done
  for ((run = 1; run <= repetitions; ++run)); do
    print_wire_to_wire_plan 1 "s1-run${run}"
  done
  for ((run = 1; run <= repetitions; ++run)); do
    print_wire_to_wire_plan 4 "s4-run${run}"
  done
  echo "+ uv run scripts/phase23_report.py --work-dir ${work_dir} --json ${report_json}" \
       "--build-flags <release flags> --load-before <load> --load-after <load>"
  echo "run_phase23_benchmarks: dry run, nothing was executed"
  exit 0
fi

# ---------------------------------------------------------------------------
# Real run.
# ---------------------------------------------------------------------------
check_idle_or_stop

log "release build in $build_dir"
./scripts/bootstrap_vcpkg.sh || exit 1
cmake --preset release || exit 1
cmake --build --preset release --target lob_gateway lob_publisher lob_loadgen spsc_latency ||
  exit 1

for binary in "$ring_binary" "$gateway_binary" "$publisher_binary" "$loadgen_binary"; do
  if [[ ! -x "$binary" ]]; then
    echo "run_phase23_benchmarks: expected binary is missing: $binary" >&2
    exit 1
  fi
done

build_flags="$(grep '^CMAKE_CXX_FLAGS_RELEASE:' "$build_dir/CMakeCache.txt" | cut -d= -f2-)"
build_flags="${build_flags:-unknown}"

rm -rf "$work_dir"
mkdir -p "$work_dir"

# The build above just made the machine busy by definition; the measured runs
# only start once the 1-minute average has settled, so the provenance row and
# the numbers describe an idle machine.
if ! wait_for_idle; then
  if [[ "$force" == 0 ]]; then
    echo "run_phase23_benchmarks: the load average did not settle below ${load_threshold}; rerun later or pass --force" >&2
    exit 1
  fi
  log "--force: starting while the load average is still above ${load_threshold}"
fi

load_before="$(load_average_1m)"
log "1-minute load average before: ${load_before}"

ring_mode=""
for ((run = 1; run <= repetitions; ++run)); do
  log "ring run ${run}/${repetitions}"
  # The first run picks the sampling mode from the measured timer; every later
  # run is pinned to it so the repetitions are comparable.
  ring_arguments=(--messages "$messages")
  if [[ -n "$ring_mode" ]]; then
    ring_arguments+=(--latency-mode "$ring_mode")
  fi
  if ! "$ring_binary" "${ring_arguments[@]}" >"$work_dir/spsc-run${run}.txt" 2>&1; then
    echo "run_phase23_benchmarks: spsc_latency failed on run ${run}; see $work_dir/spsc-run${run}.txt" >&2
    exit 1
  fi
  if [[ -z "$ring_mode" ]]; then
    ring_mode="$(sed -n 's/^mode: \([A-Za-z0-9_]*\)$/\1/p' "$work_dir/spsc-run1.txt" | head -n 1)"
    if [[ -z "$ring_mode" ]]; then
      echo "run_phase23_benchmarks: could not read the ring sampling mode" >&2
      exit 1
    fi
    log "ring sampling mode: ${ring_mode} (pinned for every repetition)"
  fi
done

for sessions in 1 4; do
  for ((run = 1; run <= repetitions; ++run)); do
    log "wire-to-wire run ${run}/${repetitions}, ${sessions} session(s)"
    label="s${sessions}-run${run}"
    if ! run_wire_to_wire "$sessions" "$work_dir/w2w-${label}.json" "$label"; then
      echo "run_phase23_benchmarks: wire-to-wire run ${label} failed; see $work_dir" >&2
      exit 1
    fi
  done
done

load_after="$(load_average_1m)"
log "1-minute load average after: ${load_after}"

if ! uv run scripts/phase23_report.py \
  --work-dir "$work_dir" \
  --json "$report_json" \
  --build-flags "$build_flags" \
  --load-before "$load_before" \
  --load-after "$load_after" \
  >"$report_markdown.tmp"; then
  echo "run_phase23_benchmarks: report rendering failed" >&2
  exit 1
fi
mv "$report_markdown.tmp" "$report_markdown"

log "wrote $report_json"
log "wrote $report_markdown"
