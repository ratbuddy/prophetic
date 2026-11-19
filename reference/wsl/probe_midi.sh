#!/usr/bin/env bash
# Inject one raw MIDI message at 12.5 s while note 60 is held (12-14 s) on the boot program and
# capture what the V55 sends the H8 (UART bytes at 0x01af72), to map MIDI controllers to the
# V55 -> H8 protocol. Usage: probe_midi.sh <label> <hex bytes, e.g. "b0 01 7f">
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LABEL="$1"; HEX="$2"
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/probe_$LABEL"
mkdir -p "$OUT"
printf "$(echo "$HEX" | sed -E 's/([0-9a-fA-F]{2}) ?/\\x\1/g')" > "$OUT/msg.mid"
cd "$OUT"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	KPROP_INJECT_SYSEX="$OUT/msg.mid:12.5" \
	KPROP_INJECT_NOTE="12:14:60:100" \
	KPDEV_H8_CALLTRACE="$OUT/calls.bin:01af72" \
	"$MAME_DIR/propmin" korgpro20 -rompath "$REPO/data/roms" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run 13 \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
