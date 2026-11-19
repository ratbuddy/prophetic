#!/usr/bin/env python3
"""Classify TMS57002 program images by instruction usage.

Usage: py tools/tms_classify.py --lst <mame>/src/devices/cpu/tms57002/tmsinstr.lst <prog.hex>...

The instruction table is read from the MAME checkout at run time (not vendored). Word layout,
per MAME's 57002dsm.cpp: fa = w >> 18; fa == 0x3f -> category 3, op = (w >> 11) & 0x7f;
otherwise one category-1 op (fa) plus one category-2 op ((w >> 11) & 0x7f) per word.
"""
from __future__ import annotations

import argparse
import re
from collections import Counter
from pathlib import Path

HEAD_RE = re.compile(r"^(\w+)\s+(1|2a|2b|3)\s+([0-9a-fA-F]{2})\s+(\d+)\s+([yn])\s*(\w*)")
BRANCHES = {"b", "bgz", "bioz", "blz", "bnz", "bv"}


def load_table(lst: Path):
    cat1, cat2, cat3 = {}, {}, {}
    lines = lst.read_text().splitlines()
    for i, line in enumerate(lines):
        m = HEAD_RE.match(line)
        if not m:
            continue
        name, typ, op = m.group(1), m.group(2), int(m.group(3), 16)
        fmt = lines[i + 1].strip() if i + 1 < len(lines) else name
        {"1": cat1, "2a": cat2, "2b": cat2, "3": cat3}[typ][op] = (name, fmt)
    return cat1, cat2, cat3


def decode(w: int, tables):
    cat1, cat2, cat3 = tables
    fa = w >> 18
    sub = (w >> 11) & 0x7F
    if fa == 0x3F:
        return [cat3.get(sub, (f"unk3_{sub:02x}", ""))] if sub else []
    ops = []
    if fa:
        ops.append(cat1.get(fa, (f"unk1_{fa:02x}", "")))
    if sub:
        ops.append(cat2.get(sub, (f"unk2_{sub:02x}", "")))
    return ops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lst", type=Path, required=True)
    ap.add_argument("progs", type=Path, nargs="+")
    args = ap.parse_args()
    tables = load_table(args.lst)
    print(f"table: cat1={len(tables[0])} cat2={len(tables[1])} cat3={len(tables[2])} entries")
    for p in args.progs:
        words = [int(x, 16) for x in p.read_text().split()]
        hist = Counter()
        branch_pcs = []
        for pc, w in enumerate(words):
            for name, _ in decode(w, tables):
                hist[name] += 1
                if name in BRANCHES:
                    branch_pcs.append((pc, name, w & 0xFF))
        idle_at = next((pc for pc, w in enumerate(words) if any(n == "idle" for n, _ in decode(w, tables))), None)
        print(f"== {p.name}: {len(words)} words, idle@{idle_at}, branches={len(branch_pcs)}, "
              f"rptk={hist['rptk']}, xram rde/wre={hist['rde']}/{hist['wre']}")
        if branch_pcs:
            print("   branches: " + ", ".join(f"{pc}:{n}->{t}" for pc, n, t in branch_pcs[:16]))
        print("   ops: " + " ".join(f"{n}:{c}" for n, c in hist.most_common()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
