#!/usr/bin/env bash
# Instruction trace of the V55 (panel CPU) around injected MIDI: how the Performance Editor knob
# routine and its helpers were found (docs/V55.md). Dev-only (firmware-derived output).
# Usage: trace_v55.sh <label> <time_s> <hex midi bytes> <trace from s> <trace to s> [seconds=12]
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LABEL="$1"; AT="$2"; HEX="$3"; T0="$4"; T1="$5"; SECS="${6:-12}"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/v55trace_$LABEL"
mkdir -p "$OUT"
echo "$HEX" | xxd -r -p > "$OUT/msg.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_SYSEX="$OUT/msg.mid:$AT" KP_TRACE_T0="$T0" KP_TRACE_T1="$T1" KP_TRACE_OUT="$OUT/v55.log" \
	"$MAME_DIR/propmin" korgpro20 -rompath "$REPO/data/roms" \
	-debug -debugger none -autoboot_script "$REPO/reference/wsl/trace_v55.lua" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run "$SECS" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || true
wc -l "$OUT/v55.log"
