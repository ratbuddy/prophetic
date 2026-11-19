#!/usr/bin/env python3
"""Oracle: inject MIDI bytes into the boot program at 11.5 s and print the V55 -> H8 messages that
follow (decoded by type nibble). Dev-only (firmware-derived output under ~/kp-traces/pe/).
Usage: [KP_TIMES=1] [KP_SECS=13] [KP_INJECT_NOTE=on:off:note:vel] probe_pe.py <repo> <label>=<hex bytes> [<label>=<hex bytes> ...]
Example: probe_pe.py /mnt/c/git/mpc-korg-prophecy k1_40="b0 13 40" pe1off="b0 63 00 b0 62 06 b0 06 00"
"""
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

repo = sys.argv[1]
runs = [a.split("=", 1) for a in sys.argv[2:]]
mame = os.environ.get("MAME_DIR", os.path.expanduser("~/src/mame-profligacy"))
root = os.path.expanduser("~/kp-traces/pe")
at = float(os.environ.get("KP_AT", "11.5"))
secs = os.environ.get("KP_SECS", "13")
LEN = {0x0: 3, 0x1: 3, 0x3: 4, 0x4: 4, 0xB: 4, 0x8: 2, 0x9: 2, 0xA: 2, 0xC: 2, 0xE: 2}   # as engine/src/synth.cpp


def decode(rx, times=None):
    out, i = [], 0
    while i < len(rx):
        t = rx[i] >> 4
        if rx[i] == 0x50 and i + 2 < len(rx):
            n = 3 + 435
        else:
            n = LEN.get(t, 1)
        out.append((times[i], bytes(rx[i:i + n])) if times else bytes(rx[i:i + n]))
        i += n
    return out


def run(arg):
    label, hexs = arg
    out = f"{root}/{label}"
    os.makedirs(out, exist_ok=True)
    open(f"{out}/msg.mid", "wb").write(bytes.fromhex(hexs))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               KPROP_INJECT_SYSEX=f"{out}/msg.mid:{at}", KPDEV_H8_CALLTRACE=f"{out}/calls.bin:01af72")
    if os.environ.get("KP_INJECT_NOTE"):   # <on s>:<off s>:<note>:<vel>, a note at its own time
        env["KPROP_INJECT_NOTE"] = os.environ["KP_INJECT_NOTE"]
    subprocess.run([f"{mame}/propmin", "korgpro20", "-rompath", f"{repo}/data/roms", "-video", "none", "-sound", "none",
                    "-nothrottle", "-skip_gameinfo", "-seconds_to_run", secs], cwd=out, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    REC = struct.Struct("<10I")
    hi = last = 0
    rx, ts = bytearray(), []
    for pc, cyc, *er in REC.iter_unpack(open(f"{out}/calls.bin", "rb").read()):
        if cyc < last:
            hi += 1 << 32
        last = cyc
        t = (hi + cyc) / 12_288_000
        if pc == 0x01AF72 and t > at - 0.05:
            rx.append(er[0] & 0xFF)
            ts.append(t)
    if os.environ.get("KP_TIMES"):   # one message per line with its time (s after the injection)
        return label, [f"{t - at:8.4f} {m.hex(' ')}" for t, m in decode(rx, ts)]
    return label, decode(rx)


with ThreadPoolExecutor(6) as ex:
    for label, msgs in ex.map(run, runs):
        if msgs and isinstance(msgs[0], str):
            print(f"{label}:\n  " + "\n  ".join(msgs))
            continue
        shown = [m.hex(" ") if len(m) < 8 else f"{m[:3].hex(' ')} <{len(m) - 3} bytes>" for m in msgs]
        print(f"{label}: " + " | ".join(shown))
