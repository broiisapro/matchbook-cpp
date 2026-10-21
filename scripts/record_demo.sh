#!/usr/bin/env bash
#
# Record the README demo to assets/demo.gif.
#
#   scripts/record_demo.sh
#
# The recording shows a generated one-million-order flow replayed on both
# engines (identical digests) and a two-million-operation differential test
# ending in `mismatches: 0`. It runs in a temporary directory with a plain `$`
# prompt and only relative command paths, so the recording contains no user
# name, home-directory path or other machine detail. The intermediate cast file
# lives in the temporary directory and is removed on exit.
#
# Environment:
#   LOB_DEMO_BUILD_DIR   build directory containing lobsim (default build/release)
#   LOB_DEMO_COLS        recorded terminal width (default 100)
#   LOB_DEMO_ROWS        recorded terminal height (default 46)
#   LOB_DEMO_FONT_SIZE   GIF font size in pixels (default 16)
#   LOB_DEMO_SPEED       GIF playback speed (default 1.5)
#
# asciinema and agg are installed with Homebrew when missing. Requires a prior
# `cmake --build --preset release`.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
assets_dir="$repo_root/assets"
build_dir="${LOB_DEMO_BUILD_DIR:-$repo_root/build/release}"

if [[ ! -d "$build_dir" ]]; then
  echo "record_demo: build directory '$build_dir' does not exist; run cmake --build --preset release" >&2
  exit 1
fi
build_dir="$(cd "$build_dir" && pwd)"
if [[ ! -x "$build_dir/lobsim" ]]; then
  echo "record_demo: '$build_dir/lobsim' not found; run cmake --build --preset release" >&2
  exit 1
fi

install_if_missing() {
  local tool="$1"
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "record_demo: installing $tool with Homebrew" >&2
    brew install "$tool"
  fi
}

install_if_missing asciinema
install_if_missing agg

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/matchbook-demo.XXXXXX")"
cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

ln -s "$build_dir/lobsim" "$work_dir/lobsim"

cat > "$work_dir/demo.sh" <<'DEMO'
#!/usr/bin/env bash
set -euo pipefail

# The recording is a script, not an interactive shell, so PS1 is not expanded;
# `prompt` prints the same plain prompt a real shell would show.
export PS1='$ '
export TERM=xterm-256color

prompt() {
  printf '\n\033[1;32m$ %s\033[0m\n' "$1"
}

run() {
  local description="$1"
  shift
  prompt "$description"
  "$@"
}

run "./lobsim gen --orders 1000000 --seed 42 --out flow.bin" \
  ./lobsim gen --orders 1000000 --seed 42 --out flow.bin

run "./lobsim replay flow.bin --engine fast --digest" \
  ./lobsim replay flow.bin --engine fast --digest

run "./lobsim replay flow.bin --engine reference --digest" \
  ./lobsim replay flow.bin --engine reference --digest

run "./lobsim difftest --ops 2000000 --seed 20261021 --progress" \
  ./lobsim difftest --ops 2000000 --seed 20261021 --progress

prompt ""
DEMO
chmod +x "$work_dir/demo.sh"

cast="$work_dir/demo.cast"
mkdir -p "$assets_dir"

# The command runs with the temporary directory as its working directory, so
# the recorded commands are exactly the relative ones above.
(
  cd "$work_dir"
  asciinema rec --headless --quiet --overwrite --return \
    --output-format asciicast-v2 \
    --window-size "${LOB_DEMO_COLS:-100}x${LOB_DEMO_ROWS:-46}" \
    --idle-time-limit 1 \
    --command "$work_dir/demo.sh" "$cast"
)

agg --quiet \
  --font-size "${LOB_DEMO_FONT_SIZE:-16}" \
  --theme github-dark \
  --idle-time-limit 1 \
  --fps-cap 20 \
  --speed "${LOB_DEMO_SPEED:-1.5}" \
  "$cast" "$assets_dir/demo.gif"

duration="$(
  awk -F',' '/^\[/ { seconds = $1; gsub(/[^0-9.]/, "", seconds); if (seconds + 0 > longest) longest = seconds + 0 } END { printf "%.1f", longest }' "$cast"
)"
bytes="$(wc -c < "$assets_dir/demo.gif" | tr -d ' ')"
printf 'record_demo: wrote %s (%s bytes, %.1f s)\n' "$assets_dir/demo.gif" "$bytes" "$duration"

if (( bytes > 3 * 1024 * 1024 )); then
  echo "record_demo: GIF exceeds 3 MB; lower LOB_DEMO_ROWS or LOB_DEMO_COLS" >&2
  exit 1
fi
if awk -v d="$duration" 'BEGIN { exit !(d > 35) }'; then
  echo "record_demo: recording is longer than 35 s; the demo should stay under ~30 s" >&2
  exit 1
fi
