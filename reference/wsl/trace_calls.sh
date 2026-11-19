#!/usr/bin/env bash
# Register snapshots at chosen H8 PCs (KPDEV_H8_CALLTRACE) over a full run with a program change
# at 11 s and a note 12-14 s. Default PCs: param_dispatch entry, its result store, dsp_cload_write.
# Usage: trace_calls.sh [set=korgpro20] [program=0] [pcs=018252,0182b0,019e0a] [seconds=15]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"; PROG="${2:-0}"; PCS="${3:-018252,0182b0,019e0a}"; SECS="${4:-15}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/calls_${SET}_p$(printf %03d "$PROG")"
mkdir -p "$OUT"
printf "\\xC0\\x$(printf %02x "$PROG")" > "$OUT/pc.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KPROP_DSP_PERFRAME=0 \
	KPROP_INJECT_SYSEX="$OUT/pc.mid:11.0" \
	KPROP_INJECT_NOTE="12:14:60:100" \
	KPDEV_H8_CALLTRACE="$OUT/calls.bin:$PCS" \
	KPDEV_REPLAY_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run "$SECS" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
ls -la "$OUT/calls.bin"
