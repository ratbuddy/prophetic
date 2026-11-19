#!/usr/bin/env bash
# One oracle run: boot, MIDI program change at 11 s, note 60 vel 100 from 12 s to 14 s, stop at 17 s.
# Usage: capture_program.sh <set> <program 0-127> <out root>
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="$1"; PROG="$2"; ROOT="$3"
OUT="$ROOT/p$(printf %03d "$PROG")"
mkdir -p "$OUT"
printf "\\xC0\\x$(printf %02x "$PROG")" > "$OUT/pc.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_SYSEX="$OUT/pc.mid:11.0" \
	KPROP_INJECT_NOTE="12:14:60:100" \
	KPDEV_HOST_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo \
	-seconds_to_run 17 -wavwrite "$OUT/out.wav" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || echo "p$PROG exited $?" >&2
