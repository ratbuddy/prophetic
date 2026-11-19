#!/usr/bin/env python3
"""Parse DSP load buffers stored in the H8 ROM (ic22) and compare them with oracle-captured programs.

Usage: rom_dsp_blocks.py <ic22.bin> <captured programs dir (*.hex)> <cpu addr>...

Buffer layout, per the H8 send routine (docs/H8.md), for DSP index 1..3:
  u16 n1, n1 bytes  -> PLOAD stream: 3-byte words ST0, ST1, program[0..]
  u16 n2, n2 bytes  -> PLOAD+CLOAD stream: 4-byte CMEM init words
Prints per buffer: header words, word counts, program SHA-1 (same hashing as trace_summary.py),
and whether that SHA-1 matches a captured program.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def cpu_view(path: Path) -> bytes:
    d = path.read_bytes()
    cpu = bytearray(len(d))
    cpu[0::2], cpu[1::2] = d[1::2], d[0::2]
    return bytes(cpu)


def parse(mem: bytes, addr: int):
    p = addr
    u16 = lambda o: int.from_bytes(mem[o:o + 2], "big")
    pre0 = pre1 = 0
    n1 = u16(p); p += 2
    pl = mem[p:p + n1]; p += n1
    n2 = u16(p); p += 2
    cm = mem[p:p + n2]; p += n2
    words = [int.from_bytes(pl[i:i + 3], "big") for i in range(0, len(pl) - len(pl) % 3, 3)]
    return pre0, pre1, words, cm, p


def main() -> int:
    mem = cpu_view(Path(sys.argv[1]))
    captured = {}
    for f in Path(sys.argv[2]).glob("*.hex"):
        words = [int(x, 16) for x in f.read_text().split()]
        sha = hashlib.sha1(b"".join(w.to_bytes(3, "big") for w in words)).hexdigest()[:12]
        captured[sha] = f.stem
    for a in sys.argv[3:]:
        addr = int(a, 16)
        pre0, pre1, words, cm, end = parse(mem, addr)
        if len(words) < 2:
            print(f"{addr:06x}: too short")
            continue
        st0, st1, prog = words[0], words[1], words[2:]
        sha = hashlib.sha1(b"".join(w.to_bytes(3, "big") for w in prog)).hexdigest()[:12]
        print(f"{addr:06x}: pre={pre0:04x},{pre1:04x} st0={st0:06x} st1={st1:06x} words={len(prog)} "
              f"cmem_bytes={len(cm)} end={end:06x} sha1={sha} -> {captured.get(sha, 'NO MATCH')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
