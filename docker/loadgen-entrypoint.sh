#!/bin/sh
#
# Entrypoint for the Compose loadgen service.
#
# The PRD's demo command is deliberately short:
#
#   docker compose run --rm loadgen --orders 1000000 --report /out/w2w.json
#
# but the process still needs the demo's network endpoints: the gateway's order
# entry port and the publisher's UDP stream and recovery port (the report's
# sequence-gap counters come from that stream). Those are supplied here so the
# command stays short, and an explicit flag on the command line always wins.
#
# Environment (Compose sets these defaults):
#   LOB_LOADGEN_GATEWAY   order-entry endpoint (default gateway:9000)
#   LOB_LOADGEN_MD_PORT   local UDP port for the market-data stream (default 9100)
#   LOB_LOADGEN_RECOVERY  publisher recovery endpoint (default publisher:9002)

set -eu

gateway="${LOB_LOADGEN_GATEWAY:-gateway:9000}"
md_port="${LOB_LOADGEN_MD_PORT:-9100}"
recovery="${LOB_LOADGEN_RECOVERY:-publisher:9002}"

has_gateway=0
has_md_port=0
has_recovery=0
previous=""
for argument in "$@"; do
  case "$previous" in
    --gateway) has_gateway=1 ;;
    --md-udp-port) has_md_port=1 ;;
    --recovery) has_recovery=1 ;;
  esac
  previous="$argument"
done

if [ "$has_recovery" -eq 0 ]; then
  set -- --recovery "$recovery" "$@"
fi
if [ "$has_md_port" -eq 0 ]; then
  set -- --md-udp-port "$md_port" "$@"
fi
if [ "$has_gateway" -eq 0 ]; then
  set -- --gateway "$gateway" "$@"
fi

exec /usr/local/bin/lob_loadgen "$@"
