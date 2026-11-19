#!/usr/bin/env python3
"""Infer the DSP serial wiring from frame-exact replay traces.

Usage: wiring_check.py <replay dir with host.dsp{1,2,3}.bin>

For each DSP input lane, report whether it is constant, else which DSP output lane it equals and at
what frame offset (in[n] == out[n - k]). 'F' records only; layout per reference patch 0001.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REC = struct.Struct("<11I")


def frames(path: Path):
    ins, outs = [], []
    data = path.read_bytes()
    for w0, frame, steps, *v in REC.iter_unpack(data):
        if (w0 & 0xFF) == ord("F"):
            ins.append(tuple(v[0:4]))
            outs.append(tuple(v[4:8]))
    return ins, outs


def main() -> int:
    d = Path(sys.argv[1])
    io = {n: frames(d / f"host.dsp{n}.bin") for n in (1, 2, 3)}
    n_frames = min(len(io[n][0]) for n in io)
    print(f"frames: {n_frames}")
    for dst in (1, 2, 3):
        ins = io[dst][0]
        for lane in range(4):
            col = [ins[f][lane] for f in range(n_frames)]
            distinct = set(col)
            if len(distinct) == 1:
                print(f"dsp{dst}.in{lane}: constant {col[0]:06x}")
                continue
            best = None
            for src in (1, 2, 3):
                outs = io[src][1]
                for sl in range(4):
                    for k in (-1, 0, 1, 2):
                        lo, hi = max(0, k), n_frames - max(0, -k)
                        tot = eq = 0
                        for f in range(lo, hi):
                            if col[f] or outs[f - k][sl]:
                                tot += 1
                                eq += col[f] == outs[f - k][sl]
                        if tot and (best is None or eq / tot > best[0]):
                            best = (eq / tot, src, sl, k, tot)
            frac, src, sl, k, tot = best
            print(f"dsp{dst}.in{lane}: == dsp{src}.out{sl} delayed {k} frame(s)  match {frac*100:.3f}% of {tot} nonzero frames  ({len(distinct)} distinct values)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
