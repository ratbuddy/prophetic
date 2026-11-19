#!/usr/bin/env bash
# Parameter-change probes: inject Prophecy SysEx PARAMETER CHANGE messages (F0 42 30 41 41 group
# idL idH vL vH F7) into the V55's MIDI in on the boot program and decode what the V55 sends the H8.
# Usage: probe_params.sh "<label>:<group>:<param id>:<value>" ...   (id may include ExID << 12)
# Output: ~/kp-traces/param_<label>/{calls.bin,uart.txt}
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
run_one() {
	IFS=: read -r label group pid val <<< "$1"
	local v=$(( val & 0x3fff )) id=$(( pid & 0x3fff ))
	local hex
	hex=$(printf "f0 42 30 41 41 %02x %02x %02x %02x %02x f7" "$group" $(( id & 0x7f )) $(( id >> 7 )) $(( v & 0x7f )) $(( v >> 7 )))
	bash "$REPO/reference/wsl/inject_capture.sh" "param_$label" 11.5 "$hex" 12.5
	local out="${KP_TRACE_ROOT:-$HOME/kp-traces}/inject_param_$label"
	{ echo "# $label group=$group id=$pid value=$val sysex: $hex"; python3 "$REPO/tools/h8_uart.py" "$out/calls.bin" --from 11.45; } > "$out/uart.txt" 2>&1
}
export -f run_one
export REPO
printf "%s\n" "$@" | xargs -P 8 -I{} bash -c 'run_one "$@"' _ {}
for a in "$@"; do cat "${KP_TRACE_ROOT:-$HOME/kp-traces}/inject_param_${a%%:*}/uart.txt"; echo; done
