#!/usr/bin/env bash
#
# Fetch the pinned vcpkg checkout and build the vcpkg binary.
#
# The pin below is the vcpkg release 2026.07.29 commit; it is also the
# builtin-baseline in vcpkg.json, so the toolchain and the manifest agree.
#
#   VCPKG_ROOT unset : cloned/verified at <repo>/vcpkg
#   VCPKG_ROOT set   : cloned/verified there, and <repo>/vcpkg is a symlink to it
#
# Idempotent (a second run with everything in place only re-checks the commit)
# and safe to run concurrently (mkdir lock with a wait loop).

set -euo pipefail

VCPKG_REPO="https://github.com/microsoft/vcpkg"
VCPKG_COMMIT="9e593bb18ea69cc5095e012465dcd675a822ed0d"

LOCK_TIMEOUT_SECONDS=900

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"

if [[ -n "${VCPKG_ROOT:-}" ]]; then
  root="$VCPKG_ROOT"
  make_symlink=1
else
  root="$repo_root/vcpkg"
  make_symlink=0
fi

log() {
  printf 'bootstrap_vcpkg: %s\n' "$*"
}

# Take the lock before touching the clone; the trap removes it on any exit.
lock="$root.lock"
mkdir -p "$(dirname "$root")"
waited=0
while ! mkdir "$lock" 2>/dev/null; do
  if (( waited % 20 == 0 )); then
    log "waiting for lock $lock (held by another bootstrap run)"
  fi
  sleep 1
  waited=$((waited + 1))
  if (( waited >= LOCK_TIMEOUT_SECONDS )); then
    log "timed out after ${LOCK_TIMEOUT_SECONDS}s waiting for $lock; if no other run is active, remove it"
    exit 1
  fi
done
trap 'rmdir "$lock" 2>/dev/null || true' EXIT

if [[ -e "$root" && ! -d "$root/.git" ]]; then
  log "error: $root exists but is not a git clone of vcpkg; move it away and rerun"
  exit 1
fi

if [[ ! -d "$root/.git" ]]; then
  log "cloning $VCPKG_REPO (full clone) into $root"
  git clone --quiet "$VCPKG_REPO" "$root"
fi

# Second runs must be fast: skip the network when the pinned commit is already
# available locally at HEAD.
if [[ "$(git -C "$root" rev-parse HEAD 2>/dev/null || true)" != "$VCPKG_COMMIT" ]]; then
  if ! git -C "$root" cat-file -e "${VCPKG_COMMIT}^{commit}" 2>/dev/null; then
    log "fetching to find pinned commit $VCPKG_COMMIT"
    git -C "$root" fetch --quiet --tags origin
  fi
  log "checking out pinned commit $VCPKG_COMMIT"
  git -C "$root" checkout --quiet --detach "$VCPKG_COMMIT"
fi

if [[ ! -x "$root/vcpkg" ]]; then
  log "building the vcpkg binary (bootstrap-vcpkg.sh -disableMetrics)"
  "$root/bootstrap-vcpkg.sh" -disableMetrics
fi

if (( make_symlink )); then
  if [[ -e "$repo_root/vcpkg" && ! -L "$repo_root/vcpkg" ]]; then
    log "error: $repo_root/vcpkg exists and is not a symlink; remove it or unset VCPKG_ROOT"
    exit 1
  fi
  ln -sfn "$root" "$repo_root/vcpkg"
fi

log "ready: $root ($(git -C "$root" rev-parse --short HEAD))"
