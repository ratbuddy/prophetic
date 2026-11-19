#!/usr/bin/env bash
# Bit-exactness gate for the standalone core: capture frame-exact traces for a set of programs in
# parallel, then replay every DSP through engine/build/replay.
# Default program set covers all 16 DSP programs (12 DSP1 oscillator sets, DSP2 filter,
# 2 DSP3 effects, clear) per data/traces/sweep_korgpro20/summary.json.
# Usage: replay_programs.sh [set=korgpro20] [programs...]
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"; shift || true
PROGS=("$@"); [ ${#PROGS[@]} -eq 0 ] && PROGS=(0 1 2 3 4 7 12 26 28 30 41 44)
ROOT="${KP_TRACE_ROOT:-$HOME/kp-traces}"
make -C "$REPO/engine" -j8 >/dev/null || exit 2
printf "%s\n" "${PROGS[@]}" | xargs -P 12 -I{} bash "$REPO/reference/wsl/capture_replay.sh" "$SET" {} >/dev/null
fail=0
for p in "${PROGS[@]}"; do
	T="$ROOT/replay_${SET}_p$(printf %03d "$p")"
	for n in 1 2 3; do
		line=$("$REPO/engine/build/replay" "$T/host.dsp$n.bin" "$n" 2>&1 | tail -1)
		rc=${PIPESTATUS[0]}
		echo "p$(printf %03d "$p") $line"
		[ "$rc" -eq 0 ] || fail=1
	done
done
echo "GATE: $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
exit $fail
