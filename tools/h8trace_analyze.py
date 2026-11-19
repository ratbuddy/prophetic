#!/usr/bin/env python3
"""Analyse an H8 PC trace (reference patch KPDEV_H8_PCTRACE: {u32 pc, u32 cycle} records).

Usage: h8trace_analyze.py <h8pc.bin> <ic22.bin> <out dir>

Writes <out>/h8_pcs.txt (unique executed PCs, hex, for h8dasm --entries) and prints:
  - instruction count, unique PCs, window length
  - each interrupt vector handler's entry count and median period (cycles, Hz at 12.288 MHz)
  - the hottest PCs
Output is firmware-derived: keep <out> under data/.
"""
from __future__ import annotations

import array
import statistics
import sys
from collections import Counter
from pathlib import Path

H8_CLOCK = 12_288_000
VEC_NAMES = {0: "reset", 7: "NMI", 8: "TRAPA0", 9: "TRAPA1", 10: "TRAPA2", 11: "TRAPA3", 12: "IRQ0", 13: "IRQ1",
             14: "IRQ2", 15: "IRQ3", 16: "IRQ4", 17: "IRQ5", 20: "WOVI", 21: "CMI", 24: "IMIA0", 25: "IMIB0",
             26: "OVI0", 28: "IMIA1", 29: "IMIB1", 30: "OVI1", 32: "IMIA2", 33: "IMIB2", 34: "OVI2", 36: "IMIA3",
             37: "IMIB3", 38: "OVI3", 40: "IMIA4", 41: "IMIB4", 42: "OVI4", 44: "DEND0A", 45: "DEND0B",
             46: "DEND1A", 47: "DEND1B", 52: "ERI0", 53: "RXI0", 54: "TXI0", 55: "TEI0", 56: "ERI1", 57: "RXI1",
             58: "TXI1", 59: "TEI1", 60: "ADI"}


def main() -> int:
    trace, rom, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    raw = array.array("I")
    raw.frombytes(trace.read_bytes())
    pcs, cyc = raw[0::2], raw[1::2]
    n = len(pcs)
    span = (cyc[-1] - cyc[0]) & 0xFFFFFFFF
    print(f"instructions: {n}, unique PCs: {len(set(pcs))}, span {span} cycles = {span / H8_CLOCK:.3f} s")
    (out / "h8_pcs.txt").write_text("".join(f"{p:06x}\n" for p in sorted(set(pcs))))

    d = rom.read_bytes()
    cpu = bytearray(len(d))
    cpu[0::2], cpu[1::2] = d[1::2], d[0::2]
    vectors = {}
    for v in range(64):
        t = int.from_bytes(cpu[v * 4:v * 4 + 4], "big") & 0xFFFFFF
        if 0x100 <= t < 0x40000 and not t & 1:
            vectors.setdefault(t, []).append(v)
    print("vector handlers (entries in window, median period):")
    for t, vs in sorted(vectors.items()):
        entries = [cyc[i] for i in range(1, n) if pcs[i] == t and pcs[i - 1] != t]
        names = ",".join(f"{v}:{VEC_NAMES.get(v, '?')}" for v in vs)
        if len(entries) > 2:
            per = statistics.median(((b - a) & 0xFFFFFFFF) for a, b in zip(entries, entries[1:]))
            print(f"  {t:06x} [{names}] entries={len(entries)} period={per:.0f} cycles = {H8_CLOCK / per:.2f} Hz")
        else:
            print(f"  {t:06x} [{names}] entries={len(entries)}")
    print("hottest PCs:")
    for pc, c in Counter(pcs).most_common(25):
        print(f"  {pc:06x} {c}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
