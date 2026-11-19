#!/usr/bin/env python3
"""Oracle expectations for the arpeggiator (engine/test/arp_test.cpp): scenarios of pattern,
octaves, pattern-parameter edits and held notes, each captured from the V55 as the note events it
sends the H8 (steady state, startup skipped). Dev-only: firmware-derived output, kept local
(default data/traces/arp_expect.txt, gitignored).
Line format: <label> pattern=<0-9> oct=<1-4> notes=<n:v,...> edits=<param:value,...> globals=<param:value,...>
cc1=<mod wheel or -> | <note>:<vel>:<gate%>:<onset in steps> ...  (globals: Global parameters,
177 / 178 = Arpeggiator Velocity / Gate Controller, 4 = CC#1)
(edits: Arpeggio Pattern Parameters of the selected pattern, Parameter Guide 6.6: 18 Step Base,
19 Sort, 22 Velocity, 25 Gate, 28 Type, 29 Octave Alternation, 33 + 4 * (step - 1) step Offset.)
Usage: arp_expect.py <repo> [out]"""
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

repo = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else f"{repo}/data/traces/arp_expect.txt"
mame = os.environ.get("MAME_DIR", os.path.expanduser("~/src/mame-profligacy"))
root = os.path.expanduser("~/kp-traces/arp_expect")
AT, SECS = 11.5, 34
LEN = {0x0: 3, 0x1: 3, 0x3: 4, 0x4: 4, 0xB: 4, 0x8: 2, 0x9: 2, 0xA: 2, 0xC: 2, 0xE: 2}   # as engine/src/synth.cpp
CEG = [(60, 100), (64, 90), (67, 80)]
ECG = [(64, 90), (60, 100), (67, 80)]
SCENARIOS = [   # label, pattern, octaves, notes, pattern edits
    ("up", 0, 1, CEG, []), ("down", 1, 1, CEG, []), ("alt1", 2, 1, CEG, []), ("alt2", 3, 1, CEG, []),
    ("up2", 0, 2, CEG, []), ("down2", 1, 2, CEG, []), ("up_unsorted", 0, 1, ECG, [(19, 0)]),
    ("pat1", 5, 1, CEG, []), ("pat2", 6, 1, CEG, []), ("pat3", 7, 1, CEG, []), ("pat4", 8, 1, CEG, []),
    ("pat5", 9, 1, CEG, []), ("pat1_o2", 5, 2, CEG, []), ("pat2_o2", 6, 2, CEG, []), ("pat3_o2", 7, 2, CEG, []),
    ("pat2_ud3", 6, 3, CEG, [(29, 2)]), ("pat2_up3", 6, 3, CEG, [(29, 0)]), ("pat2_dn3", 6, 3, CEG, [(29, 1)]),
    ("pat2_offset", 6, 1, CEG, [(37, 25), (45, -25)]), ("pat1_asplayed", 5, 1, CEG, [(28, 0)]),
    ("pat4_fill2", 8, 1, CEG[:2], []), ("pat5_unsorted", 9, 1, ECG, [(19, 0)]),
    ("pat1_vel64", 5, 1, CEG, [(22, 64)]), ("pat1_gate50", 5, 1, CEG, [(25, 50)]),
    ("pat3_16th", 7, 1, CEG, [(18, 4)]), ("pat5_updown", 9, 1, CEG, [(28, 3)]),
    # Global Arpeggiator Controller Assign (177 velocity, 178 gate; 4 = CC#1) and the mod wheel
    ("up_vel_cc64", 0, 1, CEG, [], [(177, 4)], 64), ("up_vel_cc127", 0, 1, CEG, [], [(177, 4)], 127),
    ("up_vel_neg50", 0, 1, CEG, [(24, -50)], [(177, 4)], 100), ("pat1_vel_cc90", 5, 1, CEG, [], [(177, 4)], 90),
    ("up_gate_cc64", 0, 1, CEG, [], [(178, 4)], 64), ("up_gate_neg99", 0, 1, CEG, [(27, -99)], [(178, 4)], 127),
    ("pat1_gate_neg50", 5, 1, CEG, [(27, -50)], [(178, 4)], 127), ("pat3_gate_cc40", 7, 1, CEG, [], [(178, 4)], 40),
]


def nrpn(lsb, v):
    return bytes([0xB0, 0x63, 0x00, 0xB0, 0x62, lsb, 0xB0, 0x06, v])


def param_change(group, pid, v):
    v &= 0x3FFF
    return bytes([0xF0, 0x42, 0x30, 0x41, 0x41, group, pid & 0x7F, pid >> 7, v & 0x7F, v >> 7, 0xF7])


def run(sc):
    label, pat, octv, notes, edits = sc[:5]
    glob, cc1 = (sc[5], sc[6]) if len(sc) > 5 else ([], None)
    out = f"{root}/{label}"
    os.makedirs(out, exist_ok=True)
    msg = b"".join(param_change(0, pid, v) for pid, v in glob)
    if cc1 is not None:
        msg += bytes([0xB0, 0x01, cc1])
    msg += nrpn(1, pat) + nrpn(3, octv - 1)
    for pid, v in edits:
        msg += param_change(2, pid, v)
    msg += nrpn(2, 0x7F)
    for n, v in notes:
        msg += bytes([0x90, n, v])
    open(f"{out}/msg.mid", "wb").write(msg)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               KPROP_INJECT_SYSEX=f"{out}/msg.mid:{AT}", KPDEV_H8_CALLTRACE=f"{out}/calls.bin:01af72")
    subprocess.run([f"{mame}/propmin", "korgpro20", "-rompath", f"{repo}/data/roms", "-video", "none", "-sound", "none",
                    "-nothrottle", "-skip_gameinfo", "-seconds_to_run", str(SECS)], cwd=out, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    REC = struct.Struct("<10I")
    hi = last = 0
    rx, ts = bytearray(), []
    for pc, cyc, *er in REC.iter_unpack(open(f"{out}/calls.bin", "rb").read()):
        if cyc < last:
            hi += 1 << 32
        last = cyc
        t = (hi + cyc) / 12_288_000 - AT
        if pc == 0x01AF72 and t > -0.05:
            rx.append(er[0] & 0xFF)
            ts.append(t)
    ev, i = [], 0
    while i < len(rx):
        n = 3 + 435 if rx[i] == 0x50 else LEN.get(rx[i] >> 4, 1)
        m = rx[i:i + n]
        if len(m) == 3 and m[0] in (0x10, 0x00):
            ev.append((ts[i], m[0] == 0x10, m[1], m[2]))
        i += n
    return label, sc, ev


def sequence(ev, step):
    ons = [(t, k, v) for t, on, k, v in ev if on and t > 0.3]   # skip the start (the V55 catches up)
    seq = []
    for t, k, v in ons[:-1]:
        off = next((t2 for t2, on2, k2, v2 in ev if not on2 and k2 == k and t2 > t), None)
        if off is None:
            break
        seq.append(f"{k}:{v}:{round((off - t) / step * 100)}:{(t - ons[0][0]) / step:.2f}")
    return seq


with ThreadPoolExecutor(6) as ex:
    res = list(ex.map(run, SCENARIOS))
# one step of the default Step Base (1/8) at the V55's own tempo, from the UP scenario (every gap a step)
up = next(ev for label, sc, ev in res if label == "up")
gaps = sorted(b[0] - a[0] for a, b in zip([e for e in up if e[1] and e[0] > 0.3], [e for e in up if e[1] and e[0] > 0.3][1:]))
t8 = gaps[len(gaps) // 2]
done = 0
with open(out_path, "w") as f:
    for label, sc, ev in res:
        lab, pat, octv, notes, edits = sc[:5]
        glob, cc1 = (sc[5], sc[6]) if len(sc) > 5 else ([], None)
        step = t8 * (0.5 if (18, 4) in edits else 1.0)
        seq = sequence(ev, step)
        if len(seq) < 4:
            print(f"{label}: no notes captured", file=sys.stderr)
            continue
        done += 1
        f.write(f"{label} pattern={pat} oct={octv} notes={','.join(f'{n}:{v}' for n, v in notes)} "
                f"edits={','.join(f'{p}:{v}' for p, v in edits) or '-'} "
                f"globals={','.join(f'{p}:{v}' for p, v in glob) or '-'} cc1={cc1 if cc1 is not None else '-'} "
                f"| {' '.join(seq)}\n")
print(f"wrote {out_path}: {done} scenarios, step {t8 * 1000:.2f} ms")
