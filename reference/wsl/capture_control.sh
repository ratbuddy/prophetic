#!/usr/bin/env bash
# One oracle run for control-layer verification: H8 RAM dump at T0 (default 10.9 s), frame-exact
# DSP replay traces, UART RX bytes (0x01af72), message completions (0x010cd8) and param_dispatch
# calls (0x018252, 0x0182b0, 0x019e0a). Program change at 11 s, note 12-14 s, run to 17 s.
# Usage: capture_control.sh [set=korgpro20] [program=0] [t0=10.9] [note=60]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"; PROG="${2:-0}"; T0="${3:-10.9}"; NOTE="${4:-60}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/control_${SET}_p$(printf %03d "$PROG")"
mkdir -p "$OUT"
rm -f "$OUT"/ram_*.bin
printf "\\xC0\\x$(printf %02x "$PROG")" > "$OUT/pc.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KPROP_DSP_PERFRAME=0 \
	KPROP_INJECT_SYSEX="$OUT/pc.mid:11.0" \
	KPROP_INJECT_NOTE="12:14:${NOTE}:100" \
	KPDEV_H8_RAMDUMP="$OUT/ram:$T0" \
	KPDEV_H8_CALLTRACE="$OUT/calls.bin:01af72,010cd8,018252,0182a2,0182b0,019e0a" \
	KPDEV_REPLAY_TRACE="$OUT/host" \
	"$MAME_DIR/propmin" "$SET" -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run 17 -wavwrite "$OUT/out.wav" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
ls -la "$OUT"/ram_*.bin "$OUT/calls.bin"
