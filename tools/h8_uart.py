#!/usr/bin/env python3
"""Decode V55 -> H8 UART bytes from a KPDEV_H8_CALLTRACE capture that includes PC 0x01af72
(the instruction after the SCI0 RDR read; r0l = received byte) and 0x010cd8 (message complete).

Usage: h8_uart.py <calls.bin> [--from S] [--to S] [--raw]
Prints messages split at header bytes (>= 0xf0), with time (s at 12.288 MHz) and byte count.
"""
from __future__ import annotations

import argparse
import struct
from collections import Counter
from pathlib import Path

REC = struct.Struct("<10I")
RX, DONE = 0x01AF72, 0x010CD8
CLK = 12_288_000


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("calls", type=Path)
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args()
    hi = 0
    last = 0
    msgs, cur = [], None
    for pc, cyc, *er in REC.iter_unpack(args.calls.read_bytes()):
        if cyc < last:
            hi += 1 << 32
        last = cyc
        t = (hi + cyc) / CLK
        if pc == RX:
            b = er[0] & 0xFF
            if b >= 0xF0 or cur is None:
                if cur:
                    msgs.append(cur)
                cur = {"t": t, "bytes": []}
            cur["bytes"].append(b)
        elif pc == DONE and cur:
            cur["done"] = True
    if cur:
        msgs.append(cur)
    sel = [m for m in msgs if args.t0 <= m["t"] <= args.t1]
    kinds = Counter((m["bytes"][0], len(m["bytes"])) for m in msgs)
    print(f"messages: {len(msgs)} (selected {len(sel)}); header/len histogram: "
          + ", ".join(f"{h:02x}/{n}x{c}" for (h, n), c in kinds.most_common(16)))
    for m in sel:
        b = m["bytes"]
        body = " ".join(f"{x:02x}" for x in (b if args.raw else b[:24]))
        print(f"{m['t']:9.4f}s len={len(b):4} {'D' if m.get('done') else ' '} {body}{' ...' if len(b) > 24 and not args.raw else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
