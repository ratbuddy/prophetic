#!/usr/bin/env python3
"""Oracle sweep: what does the V55 send the H8 for each SysEx PARAMETER CHANGE?

Injects, on the boot program, a blob of parameter changes (F0 42 30 41 41 01 idL idH vL vH F7),
each followed by a marker (CC#95 = running index, which the V55 forwards as `30 6f hi lo`), runs
the oracle, decodes the V55 -> H8 UART stream (PC 0x01af72) and writes one line per parameter:
the H8 messages it produced. Dev-only; output under ~/kp-traces/param_sweep/ (firmware-derived).

Usage: [GROUP=0|1|2] param_sweep.py <repo> <first> <last> [exid] [value] [label]   (GROUP: 0 global, 1 program)
"""
import os
import struct
import subprocess
import sys

repo, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
exid = int(sys.argv[4]) if len(sys.argv) > 4 else 0
value = int(sys.argv[5]) if len(sys.argv) > 5 else 1
label = sys.argv[6] if len(sys.argv) > 6 else f"{first}_{last}_x{exid}_v{value}"
mame = os.environ.get("MAME_DIR", os.path.expanduser("~/src/mame-profligacy"))
out = os.path.expanduser(f"~/kp-traces/param_sweep/{label}")
os.makedirs(out, exist_ok=True)

ids = list(range(first, last + 1))
blob = bytearray()
for k, pid in enumerate(ids):
    full = (exid << 12) | pid
    v = value & 0x3FFF
    blob += bytes([0xF0, 0x42, 0x30, 0x41, 0x41, int(os.environ.get('GROUP', '1')), full & 0x7F, (full >> 7) & 0x7F, v & 0x7F, v >> 7, 0xF7])
    blob += bytes([0xBF, 95, k & 0x7F])          # marker: CC#95 on channel 16 would be ignored; use ch 1 below
blob = blob.replace(bytes([0xBF, 95]), bytes([0xB0, 95]))
open(f"{out}/msg.syx", "wb").write(blob)
secs = 11.5 + len(blob) / 3125.0 * 2.5 + 1.5
env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
           KPROP_INJECT_SYSEX=f"{out}/msg.syx:11.5", KPDEV_H8_CALLTRACE=f"{out}/calls.bin:01af72")
subprocess.run([f"{mame}/propmin", "korgpro20", "-rompath", f"{repo}/data/roms", "-video", "none", "-sound", "none",
                "-nothrottle", "-skip_gameinfo", "-seconds_to_run", f"{secs:.1f}"], cwd=out, env=env,
               stdout=open(f"{out}/stdout.txt", "w"), stderr=open(f"{out}/stderr.txt", "w"), check=False)

REC = struct.Struct("<10I")
CLK = 12_288_000
hi = last_c = 0
rx = []
for pc, cyc, *er in REC.iter_unpack(open(f"{out}/calls.bin", "rb").read()):
    if cyc < last_c:
        hi += 1 << 32
    last_c = cyc
    if pc == 0x01AF72 and (hi + cyc) / CLK > 11.45:
        rx.append(er[0] & 0xFF)

# split the stream into V55 messages: high nibble = type; data bytes per type
NDATA = {0: 2, 1: 2, 3: 3, 4: 3, 0xB: 3, 8: 1, 9: 1, 0xA: 1, 0xC: 1, 0xE: 1}
msgs = []
i = 0
while i < len(rx):
    t = rx[i] >> 4
    if t == 5 and i + 2 < len(rx):
        n = (rx[i + 1] << 8) | rx[i + 2]
        msgs.append(bytes(rx[i:i + 3 + n]))
        i += 3 + n
    elif t in NDATA:
        msgs.append(bytes(rx[i:i + 1 + NDATA[t]]))
        i += 1 + NDATA[t]
    else:
        msgs.append(bytes(rx[i:i + 1]))
        i += 1

# attribute messages to parameters via the markers (30 6f hi lo, value = k*32767/127)
resp = {k: [] for k in range(len(ids))}
cur = None
seen = set()
for m in msgs:
    if len(m) == 4 and m[0] == 0x30 and m[1] == 0x6F:
        val = (m[2] << 8) | m[3]
        k = round(val * 127 / 32767)
        # markers repeat modulo 128: pick the next index with that residue after the current one
        base = 0 if cur is None else cur + 1
        while base < len(ids) and base % 128 != k:
            base += 1
        cur = base
        continue
    nxt = 0 if cur is None else cur + 1
    if nxt < len(ids):
        resp[nxt].append(m)
with open(f"{out}/result.txt", "w") as f:
    for k, pid in enumerate(ids):
        r = resp[k]
        kinds = " ".join(m.hex() if len(m) <= 6 else f"{m[0]:02x}[{len(m)}]" for m in r[:8])
        f.write(f"{(exid << 12) | pid:5d} exid={exid} id={pid:3d} n={len(r):3d} {kinds}{' ...' if len(r) > 8 else ''}\n")
print(f"{label}: {len(ids)} params, {len(msgs)} H8 messages -> {out}/result.txt")
