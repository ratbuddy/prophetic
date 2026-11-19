#!/usr/bin/env python3
"""Find which H8 code performs the DSP host-interface accesses, by time correlation.

Usage: h8_dsp_writers.py <trace dir with h8pc.bin and host.dsp{1,2,3}.bin>

A DSP host record at (frame, steps) happened at H8 cycle ~= frame*256 + steps/2 (H8 12.288 MHz,
DSP 24.576 MHz, 48 kHz frames). For each such record inside the PC-trace window, take the H8
instruction executing at that cycle and histogram its PC per record kind (L/C/D/R/M).
"""
from __future__ import annotations

import array
import bisect
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

REC = struct.Struct("<11I")


def main() -> int:
    d = Path(sys.argv[1])
    raw = array.array("I")
    raw.frombytes((d / "h8pc.bin").read_bytes())
    pcs, cyc = list(raw[0::2]), list(raw[1::2])
    lo, hi = cyc[0], cyc[-1]
    hist = defaultdict(Counter)
    for n in (1, 2, 3):
        for w0, frame, steps, *_ in REC.iter_unpack((d / f"host.dsp{n}.bin").read_bytes()):
            k = chr(w0 & 0xFF)
            if k == "F":
                continue
            c = frame * 256 + steps // 2
            if not lo <= c <= hi:
                continue
            i = bisect.bisect_right(cyc, c) - 1
            hist[(n, k)][pcs[max(i, 0)]] += 1
    for (n, k), h in sorted(hist.items()):
        tot = sum(h.values())
        top = ", ".join(f"{pc:06x}x{c}" for pc, c in h.most_common(8))
        print(f"dsp{n} {k}: {tot} records; H8 PC at that time: {top}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
