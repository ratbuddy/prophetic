#!/usr/bin/env bash
# Boot the user's firmware headless, inject one note after boot, write the audio to a WAV.
# Usage: reference/wsl/smoke.sh [set=korgpro20] [note=60] [seconds=16]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"
NOTE="${2:-60}"
SECS="${3:-16}"
OUT="$REPO/data/traces/smoke_${SET}_n${NOTE}"
mkdir -p "$OUT"

# Boot takes ~10 s of emulated time; note on at 11 s, off at 13 s, velocity 100.
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_NOTE="11:13:${NOTE}:100" \
	KPDEV_HOST_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo \
	-seconds_to_run "$SECS" -wavwrite "$OUT/out.wav" -log \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || { echo "propmin exited $?"; tail -20 "$OUT/stderr.txt"; exit 1; }
ls -la "$OUT"
