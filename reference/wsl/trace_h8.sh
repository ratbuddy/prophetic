#!/usr/bin/env bash
# Trace every executed H8 instruction ({u32 pc, u32 cycle}) over a time window that covers a
# program change (11 s) and a note-on (12 s), via the KPDEV_H8_PCTRACE reference patch.
# Output stays on WSL disk (firmware-derived).
# Usage: trace_h8.sh [set=korgpro20] [program=0] [start_s=10.95] [end_s=12.6]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"; PROG="${2:-0}"; START="${3:-10.95}"; END="${4:-12.6}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/h8trace_${SET}_p$(printf %03d "$PROG")"
mkdir -p "$OUT"
printf "\\xC0\\x$(printf %02x "$PROG")" > "$OUT/pc.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_SYSEX="$OUT/pc.mid:11.0" \
	KPROP_INJECT_NOTE="12:14:60:100" \
	KPDEV_H8_PCTRACE="$OUT/h8pc.bin:$START:$END" \
	KPDEV_REPLAY_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run 13 \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
ls -la "$OUT/h8pc.bin"
