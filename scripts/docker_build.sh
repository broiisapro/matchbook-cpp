#!/usr/bin/env bash
#
# Build the linux/arm64 image and smoke-test it inside the container.
#
# The image itself compiles the portable targets and runs their CTest suite in
# the build stage; this script then exercises the resulting binaries: `lobsim
# --help`, a 100k-operation differential test and the help text of the four
# Phase 3 services, so the engine, the reference engine, the tools and the
# services are checked on Linux rather than only compiled.
#
# Usage:
#   scripts/docker_build.sh            # cached build
#   scripts/docker_build.sh --no-cache # rebuild every layer
#
# Environment:
#   IMAGE      image tag (default matchbook-cpp:linux-arm64)
#   HEAVY_SH   path to a heavy-command slot gate; "none" runs Docker directly.
#              When unset, the script walks up from the repository root looking
#              for .orchestration/bin/heavy.sh and uses it if present. A plain
#              clone has no gate and runs Docker directly.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
image="${IMAGE:-matchbook-cpp:linux-arm64}"
platform="linux/arm64"

docker_args=()
for arg in "$@"; do
  case "$arg" in
    --no-cache) docker_args+=("--no-cache") ;;
    -h | --help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      printf 'docker_build: unknown argument: %s\n' "$arg" >&2
      exit 2
      ;;
  esac
done

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

log() { printf 'docker_build: %s\n' "$*"; }

run() {
  if [[ -n "$heavy" ]]; then
    "$heavy" "$@"
  else
    "$@"
  fi
}

log "building $image for $platform"
run docker build --platform "$platform" --progress=plain \
  -f "$repo_root/docker/Dockerfile" \
  ${docker_args[@]+"${docker_args[@]}"} \
  -t "$image" "$repo_root"

log "running lobsim --help in the container"
docker run --rm --platform "$platform" "$image" --help

log "running a 100k-operation differential test in the container"
difftest_output="$(docker run --rm --platform "$platform" "$image" difftest --ops 100000 --seed 1)"
printf '%s\n' "$difftest_output"
if ! printf '%s\n' "$difftest_output" | grep -qx 'mismatches: 0'; then
  log "differential test failed: expected 'mismatches: 0'"
  exit 1
fi

for binary in lob_gateway lob_publisher lob_md_listen lob_loadgen; do
  log "running $binary --help in the container"
  if ! docker run --rm --platform "$platform" --entrypoint "/usr/local/bin/$binary" "$image" --help >/dev/null; then
    log "$binary --help failed"
    exit 1
  fi
done

log "ok: $image"
