#!/usr/bin/env python3
"""Oracle expectations for the Performance Editor (engine/test/pe_test.cpp): for every factory
program, two knob moves (knobs that have switched-on PE sets, positions spread over 0..127), each
captured as the parameter edits the V55 sends after the knob's controller. Dev-only: the output is
firmware-derived and stays local (default data/traces/pe_expect.txt, gitignored).
Line format: <program> <knob 0-4> <position> <id>:<raw value byte> ...
Usage: pe_expect.py <repo> [out]"""
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

repo = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else f"{repo}/data/traces/pe_expect.txt"
mame = os.environ.get("MAME_DIR", os.path.expanduser("~/src/mame-profligacy"))
root = os.path.expanduser("~/kp-traces/pe_expect")
rom = open(f"{repo}/data/roms/korgpro20/ic12_v20.bin", "rb").read()
AT = 11.5
LEN = {0x0: 3, 0x1: 3, 0x3: 4, 0x4: 4, 0xB: 4, 0x8: 2, 0x9: 2, 0xA: 2, 0xC: 2, 0xE: 2}   # as engine/src/synth.cpp


def runs():
    for p in range(128):
        d = rom[0x3FF60 + 535 * p:0x3FF60 + 535 * (p + 1)]
        knobs = [k for k in range(5) if any(d[444 + 20 * s + 4 * k] and (d[524] >> s) & 1 for s in range(4))] or [0]
        for r in range(2):
            k = knobs[(p + r) % len(knobs)]
            x = (p * 37 + r * 64 + 11) % 128
            yield p, k, x


def run(arg):
    p, k, x = arg
    out = f"{root}/p{p:03d}_k{k}_x{x:03d}"
    os.makedirs(out, exist_ok=True)
    msg = bytes([0xB0, 0x00, 0x00, 0xB0, 0x20, p >> 6, 0xC0, p & 63, 0xB0, 19 + k, x])
    open(f"{out}/msg.mid", "wb").write(msg)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               KPROP_INJECT_SYSEX=f"{out}/msg.mid:{AT}", KPDEV_H8_CALLTRACE=f"{out}/calls.bin:01af72")
    subprocess.run([f"{mame}/propmin", "korgpro20", "-rompath", f"{repo}/data/roms", "-video", "none", "-sound", "none",
                    "-nothrottle", "-skip_gameinfo", "-seconds_to_run", "13"], cwd=out, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    REC = struct.Struct("<10I")
    hi = last = 0
    rx = bytearray()
    for pc, cyc, *er in REC.iter_unpack(open(f"{out}/calls.bin", "rb").read()):
        if cyc < last:
            hi += 1 << 32
        last = cyc
        if pc == 0x01AF72 and (hi + cyc) / 12_288_000 > AT - 0.05:
            rx.append(er[0] & 0xFF)
    # split into messages; the knob's controller message, then the edits it caused (raw value bytes)
    msgs, i = [], 0
    while i < len(rx):
        n = 3 + 435 if rx[i] == 0x50 else LEN.get(rx[i] >> 4, 1)
        msgs.append(bytes(rx[i:i + n]))
        i += n
    at = [j for j, m in enumerate(msgs) if len(m) == 4 and m[0] == 0x30 and m[1] == 0x23 + k]
    if not at:
        return p, k, x, None
    edits = []
    for m in msgs[at[-1] + 1:]:
        if len(m) != 4 or m[0] != 0x40:
            break
        edits.append(f"{(m[1] << 8) | m[2]}:{m[3]}")
    return p, k, x, edits


with ThreadPoolExecutor(6) as ex:
    res = list(ex.map(run, list(runs())))
with open(out_path, "w") as f:
    for p, k, x, edits in res:
        if edits is None:
            print(f"program {p} knob {k}: no knob controller captured", file=sys.stderr)
            continue
        f.write(f"{p} {k} {x} {' '.join(edits)}\n".replace("  ", " ").rstrip() + "\n")
print(f"wrote {out_path}: {sum(1 for r in res if r[3] is not None)} knob moves")
