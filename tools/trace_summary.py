#!/usr/bin/env python3
"""Summarise oracle host-interface traces (from the KPDEV_HOST_TRACE patch).

Usage: py tools/trace_summary.py <trace dir> [--dump]

For every <dir>/host.<dsp>.csv:
  - program loads (P rows between PLOAD assert/deassert): word count, SHA-1, ST0/ST1
  - CMEM init (I rows): count
  - coefficient updates (U rows): total, distinct addresses, per-address rate while a note sounds
--dump writes each distinct program image to <dir>/programs/<dsp>_<sha1[:12]>.hex (one 24-bit
word per line). Output stays under data/ (gitignored): it is firmware-derived.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import sys
from collections import Counter, defaultdict
from pathlib import Path


def load_rows(path: Path):
    with path.open(newline="") as f:
        for r in csv.DictReader(f):
            yield r["kind"], int(r["index"]), int(r["value"], 16), int(r["frame48k"]), float(r["seconds"])


def program_image(load: dict) -> tuple[list[int], str]:
    n = max(load["words"]) + 1
    image = [load["words"].get(i, 0) for i in range(n)]
    return image, hashlib.sha1(b"".join(w.to_bytes(3, "big") for w in image)).hexdigest()


def parse_trace(path: Path):
    """Return (loads, init_count, updates). loads: [{'t', 'words': {pc: w}, 'st': {0|1: v}}]."""
    loads = []
    cur = None
    init_count = 0
    updates = []         # (t, sa, val)
    for kind, idx, val, frame, t in load_rows(path):
        if kind == "L":
            if idx == 1:
                cur = {"t": t, "words": {}, "st": {}}
            elif cur is not None:
                if cur["words"]:
                    loads.append(cur)
                cur = None
        elif kind == "S" and cur is not None:
            cur["st"][idx] = val
        elif kind == "P" and cur is not None:
            cur["words"][idx] = val
        elif kind == "I":
            init_count += 1
        elif kind == "U":
            updates.append((t, idx, val))
    return loads, init_count, updates


def summarise(path: Path, dump_dir: Path | None) -> None:
    dsp = path.stem.split(".", 1)[1]
    loads, init_count, updates = parse_trace(path)
    print(f"== {dsp}: {len(loads)} program load(s), {init_count} CMEM init word(s), {len(updates)} update(s)")
    seen = {}
    for ld in loads:
        image, digest = program_image(ld)
        n = len(image)
        first = digest not in seen
        seen.setdefault(digest, ld["t"])
        print(f"   t={ld['t']:9.4f}s  words={n:3}  st0={ld['st'].get(0, 0):06X} st1={ld['st'].get(1, 0):06X}"
              f"  sha1={digest[:12]}{'' if first else '  (repeat)'}")
        if dump_dir and first:
            dump_dir.mkdir(parents=True, exist_ok=True)
            (dump_dir / f"{dsp}_{digest[:12]}.hex").write_text("".join(f"{w:06X}\n" for w in image))
    if updates:
        by_sa = Counter(sa for _, sa, _ in updates)
        t0, t1 = updates[0][0], updates[-1][0]
        print(f"   updates span {t0:.4f}-{t1:.4f}s, {len(by_sa)} distinct addresses")
        windows = defaultdict(int)
        for t, _, _ in updates:
            windows[int(t)] += 1
        print("   updates per emulated second: " + " ".join(f"{s}s:{n}" for s, n in sorted(windows.items())))
        top = ", ".join(f"{sa:02X}x{n}" for sa, n in by_sa.most_common(12))
        print(f"   busiest addresses: {top}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--dump", action="store_true")
    args = ap.parse_args()
    files = sorted(args.dir.glob("host.*.csv"))
    if not files:
        print(f"no host.*.csv in {args.dir}", file=sys.stderr)
        return 1
    for f in files:
        summarise(f, args.dir / "programs" if args.dump else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
