#!/usr/bin/env python3
"""Attribute an H8 PC trace to functions per time phase.

Usage: h8_phase_profile.py <h8pc.bin> <func_ranges.tsv> [phase=start:end ...]
Default phases: prog=11.0:11.6 noteon=12.0:12.05 tick=12.1:12.6 (seconds, 12.288 MHz cycles).
Prints, per function with any instructions in a phase: instruction counts per phase and the
number of distinct entries (calls) per phase.
"""
from __future__ import annotations

import array
import bisect
import sys
from collections import defaultdict
from pathlib import Path

CLK = 12_288_000


def main() -> int:
    raw = array.array("I")
    raw.frombytes(Path(sys.argv[1]).read_bytes())
    pcs, cyc = raw[0::2], raw[1::2]
    ranges = []
    for line in Path(sys.argv[2]).read_text().splitlines()[1:]:
        e, name, s, t = line.split("\t")
        ranges.append((int(s, 16), int(t, 16), int(e, 16), name))
    ranges.sort()
    starts = [r[0] for r in ranges]
    phases = {}
    for spec in (sys.argv[3:] or ["prog=11.0:11.6", "noteon=12.0:12.05", "tick=12.1:12.6"]):
        name, span = spec.split("=")
        a, b = span.split(":")
        phases[name] = (int(float(a) * CLK), int(float(b) * CLK))
    counts = defaultdict(lambda: defaultdict(int))
    entries = defaultdict(lambda: defaultdict(int))
    names = {}
    prev_fn = None
    for pc, c in zip(pcs, cyc):
        i = bisect.bisect_right(starts, pc) - 1
        fn = None
        if i >= 0 and ranges[i][0] <= pc <= ranges[i][1]:
            fn = ranges[i][2]
            names[fn] = ranges[i][3]
        for ph, (a, b) in phases.items():
            if a <= c < b:
                counts[fn][ph] += 1
                if fn is not None and pc == fn and prev_fn != fn:
                    entries[fn][ph] += 1
        prev_fn = fn
    order = sorted(counts, key=lambda f: -sum(counts[f].values()))
    print("fn      name                       " + "  ".join(f"{p:>14}" for p in phases))
    for fn in order:
        if fn is None:
            label = "(no function)"
        else:
            label = f"{fn:06x}  {names[fn][:24]:24}"
        cells = "  ".join(f"{counts[fn].get(p, 0):8}/{entries[fn].get(p, 0):<5}" for p in phases)
        print(f"{label:34} {cells}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
