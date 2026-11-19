#!/usr/bin/env bash
# Inject raw MIDI bytes at a time and capture the V55 -> H8 UART bytes (0x01af72) for the run.
# Usage: inject_capture.sh <label> <time_s> <hex bytes> [seconds=13]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LABEL="$1"; AT="$2"; HEX="$3"; SECS="${4:-13}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/inject_$LABEL"
mkdir -p "$OUT"
printf "$(echo "$HEX" | sed -E 's/([0-9a-fA-F]{2}) ?/\\x\1/g')" > "$OUT/msg.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_SYSEX="$OUT/msg.mid:$AT" \
	KPDEV_H8_CALLTRACE="$OUT/calls.bin:01af72" \
	"$MAME_DIR/propmin" korgpro20 -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run "$SECS" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
