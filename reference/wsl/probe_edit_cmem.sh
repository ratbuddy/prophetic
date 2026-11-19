#!/usr/bin/env bash
# Parameter edits on the oracle vs our engine, compared on DSP coefficient memory.
# Oracle: boot program, SysEx PARAMETER CHANGEs injected at 11.5 s (none for the base run), replay
# traces sampled at 12.3 s and 12.4 s. Ours: engine/test/synth_cmem_edits from the boot snapshot
# (10.9 s): 0.6 s, edits, then 0.8 s / 0.9 s. Only coefficients that are static over that 0.1 s in
# all four runs are compared (envelope/LFO phases move freely).
# Usage: probe_edit_cmem.sh <synth_cmem_edits binary> <replay binary> <id=value> ...
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OURS="$1"; REPLAY="$2"; shift 2
OUT="${KP_TRACE_ROOT:-$HOME/kp-traces}/edit_cmem"
rm -rf "$OUT"
mkdir -p "$OUT/base" "$OUT/edit"
python3 - "$OUT/edit/msg.syx" "$@" <<'PY'
import sys
out = bytearray()
for e in sys.argv[2:]:
    i, v = (int(x) for x in e.split("="))
    v &= 0x3fff
    out += bytes([0xF0, 0x42, 0x30, 0x41, 0x41, 0x01, i & 0x7F, (i >> 7) & 0x7F, v & 0x7F, v >> 7, 0xF7])
open(sys.argv[1], "wb").write(out)
PY
run_oracle() {
	local v="$1"
	cd "$OUT/$v"
	if [ "$v" = edit ]; then export KPROP_INJECT_SYSEX="$OUT/$v/msg.syx:11.5"; else unset KPROP_INJECT_SYSEX; fi
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KPROP_DSP_PERFRAME=0 KPDEV_REPLAY_TRACE="$OUT/$v/host" \
		"$MAME_DIR/propmin" korgpro20 -rompath "$REPO/data/roms" -video none -sound none -nothrottle \
		-skip_gameinfo -seconds_to_run 12.5 > stdout.txt 2> stderr.txt
	for t in 590400 595200; do
		for d in 1 2 3; do "$REPLAY" host.dsp$d.bin $d $t cm$d.bin > /dev/null 2>&1 || true; done
		cat cm1.bin cm2.bin cm3.bin > oracle_$t.bin
	done
}
( run_oracle base ) &
( run_oracle edit ) &
for t in 0.8 0.9; do
	"$OURS" "$REPO/out/win/data" 0.6 $t "$OUT/ours_base_$t.bin" 2>/dev/null &
	"$OURS" "$REPO/out/win/data" 0.6 $t "$OUT/ours_edit_$t.bin" "$@" 2>/dev/null &
done
wait
python3 - "$OUT" <<'PY'
import sys
o = sys.argv[1]
r = lambda p: open(p, "rb").read()
w = lambda b, k: b[4 * k:4 * k + 4].hex()
ob, oe = r(f"{o}/base/oracle_595200.bin"), r(f"{o}/edit/oracle_595200.bin")
ob0, oe0 = r(f"{o}/base/oracle_590400.bin"), r(f"{o}/edit/oracle_590400.bin")
ub, ue = r(f"{o}/ours_base_0.9.bin"), r(f"{o}/ours_edit_0.9.bin")
ub0, ue0 = r(f"{o}/ours_base_0.8.bin"), r(f"{o}/ours_edit_0.8.bin")
static = {k for k in range(768) if w(ob, k) == w(ob0, k) and w(oe, k) == w(oe0, k) and w(ub, k) == w(ub0, k) and w(ue, k) == w(ue0, k)}
print(f"static coefficient words: {len(static)} of 768; base runs equal on {sum(w(ob, k) == w(ub, k) for k in static)}, edited runs equal on {sum(w(oe, k) == w(ue, k) for k in static)}")
oc = {k for k in static if w(ob, k) != w(oe, k)}
uc = {k for k in static if w(ub, k) != w(ue, k)}
agree = [k for k in oc & uc if w(oe, k) == w(ue, k)]
print(f"static coefficients changed by the edits: oracle {len(oc)}, ours {len(uc)}, both {len(oc & uc)}, same new value {len(agree)}")
for k in sorted(oc ^ uc)[:16]:
    print(f"  only {'oracle' if k in oc else 'ours  '}: dsp{k // 256 + 1} cmem[{k % 256:3d}] oracle {w(ob, k)}->{w(oe, k)}  ours {w(ub, k)}->{w(ue, k)}")
for k in sorted(set(oc & uc) - set(agree))[:16]:
    print(f"  differ     : dsp{k // 256 + 1} cmem[{k % 256:3d}] oracle {w(ob, k)}->{w(oe, k)}  ours {w(ub, k)}->{w(ue, k)}")
PY
