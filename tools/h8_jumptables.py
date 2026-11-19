#!/usr/bin/env python3
"""Find H8 function-pointer tables and list their targets as translation entries.

Usage: h8_jumptables.py <ic22.bin> <ic22.ir> <out entries.txt>

A table is any `mov.l @(TABLE, erN), erM` whose erM feeds a `jsr @erM` / `jmp @erM` within the next
6 instructions. Entries are read from TABLE while they are even code pointers in 0x10000-0x3FFFF
(capped at 256). Prints each table and its entry count.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract import cpu_view  # noqa: E402


def main() -> int:
    rom = cpu_view(Path(sys.argv[1]))[:0x40000]
    rows = [l.split("\t") for l in Path(sys.argv[2]).read_text().splitlines() if not l.startswith("FUNC")]
    tables = {}
    for i, r in enumerate(rows):
        if r[3] != "mov.l" or len(r) < 6 or not r[4].startswith("DISP:") or not r[5].startswith("R32:"):
            continue
        base = int(r[4].split(":")[2], 16)
        reg = r[5].split(":")[1]
        if not (0x100 <= base < 0x40000):
            continue
        for nxt in rows[i + 1:i + 7]:
            if nxt[3] in ("jsr", "jmp") and nxt[4:] == [f"R32:{reg}"]:
                tables.setdefault(base, set()).add(r[0])
                break
    def run(base, stride):
        out = []
        while len(out) < 256:
            a = base + stride * len(out)
            v = int.from_bytes(rom[a:a + 4], "big")
            if not (0x10000 <= v < 0x40000) or v & 1:
                break
            out.append(v)
        return out

    entries = set()
    for base in sorted(tables):
        stride, best = max(((s, run(base, s)) for s in (4, 6, 8, 10, 12)), key=lambda sb: (len(sb[1]), -sb[0]))
        entries.update(best)
        print(f"table {base:06x}: {len(best)} entries, stride {stride} (used at {', '.join(sorted(tables[base]))})")
    Path(sys.argv[3]).write_text("".join(f"{e:06x}\n" for e in sorted(entries)))
    print(f"{len(entries)} distinct targets -> {sys.argv[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
