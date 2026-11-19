#!/usr/bin/env python3
"""Bring-your-own-ROM extraction: decode the DSP programs from the user's H8 ROM.

Usage: py tools/extract.py [--set korgpro20]

Reads romdata/<set>.json (addresses only, committed) and data/roms/<set>/<ic22> (user's dump,
gitignored); writes data/extracted/<set>/dsp_programs.json (firmware-derived, gitignored):
  {"osc": [prog...12], "fx": [prog...2], "filter": prog, "clear": prog}
  prog = {"rom": addr, "st0": int, "st1": int, "words": [24-bit...], "cmem": [32-bit x 256], "sha1": str}
ROM buffer layout (send routine, DSP index 1-3): u16 n1, n1 bytes PLOAD (3-byte words ST0, ST1,
program...), u16 n2, n2 bytes CMEM init (4-byte words).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def cpu_view(path: Path) -> bytes:
    d = path.read_bytes()
    cpu = bytearray(len(d))
    cpu[0::2], cpu[1::2] = d[1::2], d[0::2]
    return bytes(cpu)


def u16(m: bytes, a: int) -> int:
    return int.from_bytes(m[a:a + 2], "big")


def u32(m: bytes, a: int) -> int:
    return int.from_bytes(m[a:a + 4], "big")


def decode_buffer(m: bytes, addr: int) -> dict:
    n1 = u16(m, addr)
    pl = m[addr + 2:addr + 2 + n1]
    n2 = u16(m, addr + 2 + n1)
    cm = m[addr + 4 + n1:addr + 4 + n1 + n2]
    if n1 % 3 or n1 < 9 or n1 > 3 * 258 or n2 not in (0, 1024):
        raise ValueError(f"buffer {addr:06x}: implausible sizes n1={n1} n2={n2}")
    words = [int.from_bytes(pl[i:i + 3], "big") for i in range(0, n1, 3)]
    prog = words[2:]
    return {
        "rom": f"0x{addr:06x}",
        "st0": words[0],
        "st1": words[1],
        "words": prog,
        "cmem": [int.from_bytes(cm[i:i + 4], "big") for i in range(0, n2, 4)],
        "sha1": hashlib.sha1(b"".join(w.to_bytes(3, "big") for w in prog)).hexdigest(),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default="korgpro20")
    args = ap.parse_args()
    cfg = json.loads((REPO / "romdata" / f"{args.set}.json").read_text())
    rom = REPO / "data" / "roms" / args.set / cfg["ic22"]
    if not rom.exists():
        print(f"missing {rom}; run tools/check_roms.py <dir> --install first", file=sys.stderr)
        return 1
    m = cpu_view(rom)
    d = cfg["dsp"]
    table = lambda t: [decode_buffer(m, u32(m, int(t["addr"], 16) + 4 * i)) for i in range(t["count"])]
    out = {
        "set": args.set,
        "osc": table(d["osc_table"]),
        "fx": table(d["fx_table"]),
        "filter": decode_buffer(m, int(d["filter"]["addr"], 16)),
        "clear": decode_buffer(m, int(d["clear"]["addr"], 16)),
    }
    dest = REPO / "data" / "extracted" / args.set
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "dsp_programs.json").write_text(json.dumps(out))
    for k in ("osc", "fx"):
        for i, p in enumerate(out[k]):
            print(f"{k}[{i:2}] {p['rom']} words={len(p['words']):3} sha1={p['sha1'][:12]}")
    for k in ("filter", "clear"):
        p = out[k]
        print(f"{k:7} {p['rom']} words={len(p['words']):3} sha1={p['sha1'][:12]}")
    print(f"wrote {dest / 'dsp_programs.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
