#!/usr/bin/env bash
#
# End-to-end wiring test for the Phase 3 processes (decision C58, issue #153).
#
#   scripts/e2e_local.sh GATEWAY PUBLISHER MDLISTEN LOADGEN [orders] [sessions]
#
# Starts the gateway, the publisher, a market-data listener and the load
# generator on loopback, sends `orders` requests (default 50000) from two
# sessions through the real TCP order-entry path, and fails unless every order
# was answered and the load generator saw no unrecovered market-data sequence
# gap. Used by the E2E.LocalWiring ctest test; also runnable by hand against
# any build directory.
#
# Every port is chosen at run time; if a chosen port turns out to be taken by
# the time a process binds it, the whole run is retried with fresh ports. The
# script stops exactly the processes it started, on every exit path: it records
# their PIDs and never looks processes up by name.
#
# Environment:
#   E2E_READY_TIMEOUT  seconds to wait for each process to be ready (default 20)
#   E2E_ATTEMPTS       port-collision attempts before giving up (default 5)
#   E2E_KEEP_LOGS      set to 1 to always keep the temporary log directory

set -uo pipefail

if [[ $# -lt 4 || $# -gt 6 ]]; then
  sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

gateway_bin="$1"
publisher_bin="$2"
mdlisten_bin="$3"
loadgen_bin="$4"
orders="${5:-50000}"
sessions="${6:-2}"
ready_timeout="${E2E_READY_TIMEOUT:-20}"
attempts="${E2E_ATTEMPTS:-5}"

for binary in "$gateway_bin" "$publisher_bin" "$mdlisten_bin" "$loadgen_bin"; do
  if [[ ! -x "$binary" ]]; then
    echo "e2e_local: not executable: $binary" >&2
    exit 2
  fi
done
for number in "$orders" "$sessions"; do
  if ! [[ "$number" =~ ^[1-9][0-9]*$ ]]; then
    echo "e2e_local: orders and sessions must be positive integers, got $number" >&2
    exit 2
  fi
done
for number in "$ready_timeout" "$attempts"; do
  if ! [[ "$number" =~ ^[1-9][0-9]*$ ]]; then
    echo "e2e_local: E2E_READY_TIMEOUT and E2E_ATTEMPTS must be positive integers, got $number" >&2
    exit 2
  fi
done
if ! command -v python3 >/dev/null 2>&1; then
  echo "e2e_local: python3 is required (port selection and report checks)" >&2
  exit 2
fi

log() { printf 'e2e_local: %s\n' "$*"; }

# One free loopback port per requested socket. TCP and UDP port spaces are
# independent on macOS and Linux, but the kernel assigns both from the same
# range, so a port free for TCP is almost always free for UDP too; a bind-time
# collision triggers the retry loop below.
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

# True while the port accepts a TCP connection from loopback.
tcp_open() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

# True while some process holds the UDP port on loopback. The probe binds
# without SO_REUSEADDR, so an already-bound port fails with EADDRINUSE.
udp_bound() {
  python3 - "$1" <<'PY'
import errno
import socket
import sys

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    sock.bind(("127.0.0.1", int(sys.argv[1])))
except OSError as error:
    sys.exit(0 if error.errno == errno.EADDRINUSE else 1)
finally:
    sock.close()
sys.exit(1)
PY
}

# wait_until PID DESCRIPTION COMMAND...: polls COMMAND every 20 ms until it
# succeeds, the timeout elapses, or PID exits. The process is checked first so
# a crashed process is reported immediately instead of after the timeout.
wait_until() {
  local pid="$1"
  local description="$2"
  shift 2
  local deadline=$((SECONDS + ready_timeout))
  while true; do
    if ! process_running "$pid"; then
      log "${description}: process exited before it was ready"
      return 1
    fi
    if "$@"; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      log "${description}: not ready after ${ready_timeout}s"
      return 1
    fi
    sleep 0.02
  done
}

# Every process the script started, so cleanup never has to guess. start() sets
# started_pid; it runs in the current shell, never in a subshell.
children=()
started_pid=0
logs_dir=""
report_path=""

# start NAME BINARY ARGUMENTS...: starts one process with its output in the log
# directory and records its PID in started_pid.
start() {
  local name="$1"
  shift
  "$@" >"$logs_dir/$name.log" 2>&1 &
  started_pid=$!
  children+=("$started_pid")
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

# forget_pid PID: drops a process that has already been reaped, so its PID can
# never be signalled after the kernel reuses it.
forget_pid() {
  local keep=()
  local pid
  for pid in "${children[@]}"; do
    [[ -z "$pid" || "$pid" == "$1" ]] || keep+=("$pid")
  done
  children=("${keep[@]}")
}

# True when a process log says its listen/connect failed because the port was
# taken between free_ports and the bind. Only then is a retry with fresh ports
# worth doing.
failed_to_bind() {
  grep -qi "address already in use" "$logs_dir"/*.log 2>/dev/null
}

# True while PID is a live process. `kill -0` alone also succeeds for a zombie
# this shell has not reaped yet, so the process state is checked instead.
process_running() {
  local state
  state="$(ps -o state= -p "$1" 2>/dev/null)"
  [[ -n "$state" && "$state" != Z* ]]
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM HUP
  stop_children
  if [[ -n "$logs_dir" && -d "$logs_dir" ]]; then
    if (( status == 0 )) && [[ "${E2E_KEEP_LOGS:-0}" != "1" ]]; then
      rm -rf "$logs_dir"
    else
      log "logs kept in $logs_dir"
    fi
  fi
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 129' HUP
trap 'exit 143' TERM

# run_once: one full wiring attempt. Returns 75 when a process could not bind
# its port (the caller retries with fresh ports), 0 when the run passed, and 1
# for any other failure.
run_once() {
  local gateway_port md_port recovery_port mdlisten_udp loadgen_udp
  read -r gateway_port md_port recovery_port mdlisten_udp loadgen_udp < <(free_ports 5) || return 1
  if [[ -z "$loadgen_udp" ]]; then
    log "could not allocate loopback ports"
    return 1
  fi

  logs_dir="$(mktemp -d "${TMPDIR:-/tmp}/lob-e2e.XXXXXX")" || return 1
  report_path="$logs_dir/report.json"
  log "ports: entry ${gateway_port}, md ${md_port}, recovery ${recovery_port}," \
      "md-listener ${mdlisten_udp}, loadgen ${loadgen_udp}"

  start gateway "$gateway_bin" --port "$gateway_port" --md-port "$md_port"
  local gateway_pid=$started_pid
  wait_until "$gateway_pid" "gateway" tcp_open "$gateway_port" ||
    return "$(failed_to_bind && echo 75 || echo 1)"
  wait_until "$gateway_pid" "gateway md stream" tcp_open "$md_port" ||
    return "$(failed_to_bind && echo 75 || echo 1)"

  start publisher "$publisher_bin" \
    --gateway "127.0.0.1:$md_port" \
    --udp-dest "127.0.0.1:$mdlisten_udp" \
    --udp-dest "127.0.0.1:$loadgen_udp" \
    --recovery-port "$recovery_port"
  local publisher_pid=$started_pid
  wait_until "$publisher_pid" "publisher" tcp_open "$recovery_port" ||
    return "$(failed_to_bind && echo 75 || echo 1)"
  wait_until "$publisher_pid" "publisher gateway connection" \
    grep -q "connected to gateway" "$logs_dir/publisher.log" ||
    return "$(failed_to_bind && echo 75 || echo 1)"

  start md_listen "$mdlisten_bin" \
    --udp-port "$mdlisten_udp" \
    --bind 127.0.0.1 \
    --recovery "127.0.0.1:$recovery_port" \
    --top 5
  local mdlisten_pid=$started_pid
  wait_until "$mdlisten_pid" "md_listen" udp_bound "$mdlisten_udp" ||
    return "$(failed_to_bind && echo 75 || echo 1)"

  log "running $orders orders over $sessions session(s)"
  # The load generator runs in the background so a signal to this script stops
  # it too, instead of waiting for the whole load to finish first.
  local started_at=$SECONDS
  local loadgen_pid loadgen_status=0
  "$loadgen_bin" \
    --gateway "127.0.0.1:$gateway_port" \
    --orders "$orders" \
    --sessions "$sessions" \
    --report "$report_path" \
    --md-udp-port "$loadgen_udp" \
    --recovery "127.0.0.1:$recovery_port" \
    >"$logs_dir/loadgen.log" 2>&1 &
  loadgen_pid=$!
  children+=("$loadgen_pid")
  wait "$loadgen_pid" || loadgen_status=$?
  forget_pid "$loadgen_pid"
  cat "$logs_dir/loadgen.log"
  if (( loadgen_status != 0 )); then
    log "load generator exited with status ${loadgen_status}; report is in $report_path"
    failed_to_bind && return 75
    return 1
  fi
  local elapsed=$((SECONDS - started_at))

  # A process that died mid-run leaves a clean-looking latency report, so the
  # long-running members of the topology are checked explicitly.
  local name pid
  for name in gateway publisher md_listen; do
    case "$name" in
      gateway) pid=$gateway_pid ;;
      publisher) pid=$publisher_pid ;;
      md_listen) pid=$mdlisten_pid ;;
    esac
    if ! process_running "$pid"; then
      log "${name} exited during the run; see $logs_dir/${name}.log"
      return 1
    fi
  done

  python3 - "$report_path" "$elapsed" <<'PY' || return 1
import json
import sys

with open(sys.argv[1]) as handle:
    report = json.load(handle)

problems = []
if report.get("orders_without_response") != 0:
    problems.append(f"{report.get('orders_without_response')} order(s) without a response")
if report.get("timed_out"):
    problems.append("the run timed out")
md = report.get("md")
if not isinstance(md, dict):
    problems.append("the report has no market-data block")
else:
    if md.get("received", 0) <= 0:
        problems.append("no market-data frames were received")
    gap_fields = [key for key in ("sequence_gaps_unrecovered", "unrecovered_gaps") if key in md]
    if not gap_fields:
        problems.append("the market-data block has no unrecovered-gap counter")
    for key in gap_fields:
        if md[key] != 0:
            problems.append(f"{md[key]} unrecovered market-data gap(s) ({key})")

if problems:
    sys.exit("e2e_local: " + "; ".join(problems))

gaps = max(md.get("sequence_gaps_unrecovered", 0), md.get("unrecovered_gaps", 0))
print(
    f"e2e_local: ok - {report.get('orders_requested')} orders, "
    f"{report.get('sessions')} session(s), 0 unanswered, {gaps} unrecovered gap(s), "
    f"{md.get('received')} market-data frames, final seq {md.get('final_seq', 0)}, "
    f"{report.get('elapsed_seconds'):.2f}s load, {sys.argv[2]}s wall"
)
PY
  stop_children
  return 0
}

# A failed attempt may have left a log directory behind; the retry should not
# accumulate them. The final attempt's directory is handled by cleanup.
discard_last_logs() {
  if [[ "${E2E_KEEP_LOGS:-0}" != "1" && -n "$logs_dir" && -d "$logs_dir" ]]; then
    rm -rf "$logs_dir"
  fi
  logs_dir=""
}

for ((attempt = 1; attempt <= attempts; ++attempt)); do
  run_once
  status=$?
  if (( status == 0 )); then
    exit 0
  fi
  stop_children
  if (( status != 75 )); then
    exit "$status"
  fi
  discard_last_logs
  log "attempt ${attempt}/${attempts} lost a port to another process; retrying"
done

log "gave up after ${attempts} attempts"
exit 1
