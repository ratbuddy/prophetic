#!/usr/bin/env python3
"""Extract per-program V55 -> H8 program-change bursts from oracle UART traces.

Usage: extract_program_bursts.py <sweep root with calls_<set>_pNNN/calls.bin> <set> <out programs dir>

Each run changed program at 11.0 s; the burst is every UART byte received between 10.95 s and
11.95 s (controllers, the type-5 program record, follow-up messages). Writes NNN.uart (raw bytes)
and NNN.name (16-char name from the type-5 record). Output is firmware-derived: keep it under data/.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REC = struct.Struct("<10I")
CLK = 12_288_000


def main() -> int:
    root, set_name, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for run in sorted(root.glob(f"calls_{set_name}_p[0-9][0-9][0-9]")):
        f = run / "calls.bin"
        if not f.exists():
            continue
        prog = int(run.name[-3:])
        burst = [er[0] & 0xFF for pc, c, *er in REC.iter_unpack(f.read_bytes())
                 if pc == 0x01AF72 and 10.95 <= c / CLK < 11.95]
        name = "?"
        for i in range(len(burst) - 19):
            if burst[i] >> 4 == 5 and (burst[i + 1] << 8 | burst[i + 2]) == 0x1B3:
                name = bytes(burst[i + 3:i + 19]).decode("ascii", "replace").rstrip()
                break
        (out / f"{prog:03d}.uart").write_bytes(bytes(burst))
        (out / f"{prog:03d}.name").write_text(name)
        print(f"{prog:03d} {len(burst):4} bytes  {name}")
        n += 1
    print(f"{n} programs -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
