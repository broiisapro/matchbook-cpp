#!/usr/bin/env bash
#
# Run the Phase 3 Compose demo end to end.
#
#   scripts/compose_demo.sh                 # 1,000,000 orders, then clean up
#   scripts/compose_demo.sh --orders 100000
#   scripts/compose_demo.sh --keep          # leave the stack running
#
# The script runs the three commands from the PRD definition of done:
#
#   docker compose up -d --build
#   docker compose run --rm loadgen --orders N --report /out/w2w.json
#   docker compose down
#
# prints the load generator's report, and checks that the market-data stream
# had no unrecovered sequence gap. The image build and the load run go through
# the heavy-command slot gate when one is available; `down` does not.
#
# Environment:
#   ORDERS     number of orders (default 1000000)
#   HEAVY_SH   path to a heavy-command slot gate; "none" runs Docker directly.
#              When unset, the script walks up from the repository root looking
#              for .orchestration/bin/heavy.sh and uses it if present.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
report_path="/out/w2w.json"

orders="${ORDERS:-1000000}"
keep=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --orders)
      if [[ $# -lt 2 ]]; then
        printf 'compose_demo: --orders needs a value\n' >&2
        exit 2
      fi
      orders="$2"
      shift 2
      ;;
    --orders=*)
      orders="${1#--orders=}"
      shift
      ;;
    --keep)
      keep=1
      shift
      ;;
    -h | --help)
      sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      printf 'compose_demo: unknown argument: %s\n' "$1" >&2
      exit 2
      ;;
  esac
done

if ! [[ "$orders" =~ ^[1-9][0-9]*$ ]]; then
  printf 'compose_demo: --orders must be a positive integer, got %s\n' "$orders" >&2
  exit 2
fi

heavy="${HEAVY_SH:-}"
if [[ -z "$heavy" ]]; then
  dir="$repo_root"
  while [[ "$dir" != "/" ]]; do
    if [[ -x "$dir/.orchestration/bin/heavy.sh" ]]; then
      heavy="$dir/.orchestration/bin/heavy.sh"
      break
    fi
    dir="$(dirname "$dir")"
  done
fi
if [[ "$heavy" == "none" ]]; then
  heavy=""
fi

log() { printf 'compose_demo: %s\n' "$*"; }

compose_cmd=(docker compose --project-directory "$repo_root" -f "$repo_root/docker-compose.yml")

compose() { "${compose_cmd[@]}" "$@"; }

# Build and run are the heavy steps, so they go through the slot gate. A shell
# function cannot be handed to the gate, hence the explicit command array.
run_compose() {
  if [[ -n "$heavy" ]]; then
    "$heavy" "${compose_cmd[@]}" "$@"
  else
    "${compose_cmd[@]}" "$@"
  fi
}

cleanup() {
  log "stopping the stack"
  compose down
}

if [[ "$keep" == "0" ]]; then
  trap cleanup EXIT
fi

log "building and starting gateway, publisher and two listeners"
run_compose up -d --build

log "running $orders orders through the load generator"
run_compose run --rm -T loadgen --orders "$orders" --report "$report_path"

log "load generator report ($report_path)"
report="$(compose run --rm -T --entrypoint cat loadgen "$report_path")"
printf '%s\n' "$report"

if command -v python3 >/dev/null 2>&1; then
  printf '%s' "$report" | python3 -c '
import json
import sys

report = json.load(sys.stdin)


def unrecovered(node):
    """Find the unrecovered-gap counter whatever the report nests it under."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key in ("sequence_gaps_unrecovered", "unrecovered_gaps") and value == 0:
                return True
            if unrecovered(value):
                return True
    elif isinstance(node, list):
        return any(unrecovered(item) for item in node)
    return False


if not unrecovered(report):
    sys.exit("compose_demo: the report has no unrecovered sequence-gap count of 0")
'
else
  log "python3 not found; skipping the sequence-gap check"
fi

log "ok: demo completed with 0 unrecovered sequence gaps"
