#!/usr/bin/env python3
"""Oracle: the program record the V55 re-sends when OSC Set changes (boot program, Set 4 =
Standard + Modulation), for each target set 0..11. Prints each record's OSC1/OSC2 setting regions
(record bytes 172..205 and 220..253). Dev-only (firmware-derived output under ~/kp-traces/).
Usage: probe_oscsets.py <repo>"""
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

repo = sys.argv[1]
mame = os.environ.get("MAME_DIR", os.path.expanduser("~/src/mame-profligacy"))
root = os.path.expanduser("~/kp-traces/oscsets")


def run(s):
    out = f"{root}/set{s:02d}"
    os.makedirs(out, exist_ok=True)
    msg = bytes([0xF0, 0x42, 0x30, 0x41, 0x41, 0x01, 154 & 0x7F, 154 >> 7, s, 0, 0xF7])
    open(f"{out}/msg.syx", "wb").write(msg)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               KPROP_INJECT_SYSEX=f"{out}/msg.syx:11.5", KPDEV_H8_CALLTRACE=f"{out}/calls.bin:01af72")
    subprocess.run([f"{mame}/propmin", "korgpro20", "-rompath", f"{repo}/data/roms", "-video", "none", "-sound", "none",
                    "-nothrottle", "-skip_gameinfo", "-seconds_to_run", "12.4"], cwd=out, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    REC = struct.Struct("<10I")
    hi = last = 0
    rx = bytearray()
    for pc, cyc, *er in REC.iter_unpack(open(f"{out}/calls.bin", "rb").read()):
        if cyc < last:
            hi += 1 << 32
        last = cyc
        if pc == 0x01AF72 and (hi + cyc) / 12_288_000 > 11.45:
            rx.append(er[0] & 0xFF)
    i = rx.find(bytes([0x50, 0x01, 0xB3]))
    rec = bytes(rx[i + 3:i + 3 + 435]) if i >= 0 else b""
    tail = bytes(rx[i + 3 + 435:i + 3 + 435 + 64]) if i >= 0 else b""
    return s, rec, tail


with ThreadPoolExecutor(6) as ex:
    res = list(ex.map(run, range(12)))
for s, rec, tail in res:
    if len(rec) != 435:
        print(f"set {s + 1}: no record")
        continue
    open(f"{root}/set{s:02d}/record.bin", "wb").write(rec)
    print(f"set {s + 1:2d}: byte138={rec[138]} osc1 {rec[172:206].hex()} osc2 {rec[220:254].hex()}")
print("tail after record (set 1):", res[0][2].hex(" "))
