#!/usr/bin/env bash
# Frame-exact replay capture (interpreter forced): boot, program change at 11 s, note 12-14 s.
# Usage: capture_replay.sh <set> <program 0-127> [seconds=17] [note=60]
# Output: $KP_TRACE_ROOT/replay_<set>_pNNN/host.dsp{1,2,3}.bin + out.wav (WSL native disk)
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="$1"; PROG="$2"; SECS="${3:-17}"; NOTE="${4:-60}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/replay_${SET}_p$(printf %03d "$PROG")"
mkdir -p "$OUT"
printf "\\xC0\\x$(printf %02x "$PROG")" > "$OUT/pc.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_DSP_PERFRAME=0 \
	KPROP_INJECT_SYSEX="$OUT/pc.mid:11.0" \
	KPROP_INJECT_NOTE="12:14:${NOTE}:100" \
	KPDEV_REPLAY_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo \
	-seconds_to_run "$SECS" -wavwrite "$OUT/out.wav" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
ls -la "$OUT"
