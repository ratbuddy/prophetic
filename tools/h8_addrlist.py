#!/usr/bin/env python3
"""H8 address lists (romdata/<set>_h8_seeds.txt, romdata/<set>_h8_entries.txt), expanded against your ic22.

A list holds one H8 code address per line (hex) and "table <addr> <count> <stride>" lines: <count>
big-endian 32-bit pointers, <stride> bytes apart, that the build reads from your ic22 at <addr>
(hex, CPU byte order). The ROM's pointer arrays and {key, pointer} dispatch records are ascending, so
a sorted list holding their targets one after another would reproduce them; table lines keep every
such stretch out of the repo. '#' starts a comment.

Usage:
  py tools/h8_addrlist.py expand <ic22.bin> <list.txt> -o <out.txt>   the build: one address per line, sorted
  py tools/h8_addrlist.py pack <ic22.bin> <list.txt> -o <list.txt>    dev: expand, then move every stretch of
                                                                      >= 2 into table lines
  py tools/h8_addrlist.py check <ic22.bin> <list.txt>                 dev: the longest stretch of the list's own
                                                                      addresses that lies in the ROM in list order
<ic22.bin> is the dump as made (word-swapped relative to the CPU, as tools/h8dec.py reads it).
Exit 0 = ok, 1 = check found a stretch of 2 or more, 2 = usage or input error.
"""
from __future__ import annotations

import sys
from pathlib import Path

HEADER = ("# H8 code addresses for tools/h8dec.py and tools/h8xlat.py, expanded by tools/h8_addrlist.py:\n"
          "# one address per line (hex), or \"table <addr> <count> <stride>\": <count> 32-bit pointers,\n"
          "# <stride> bytes apart, read from your ic22 at <addr> (CPU byte order). Addresses, counts\n"
          "# and strides only, no ROM content.\n")
MAX_STRIDE = 32


def cpu_image(dump: bytes) -> bytes:
    """The dump in CPU byte order (h8dec.py's unswap)."""
    n = len(dump) - (len(dump) & 1)
    out = bytearray(n)
    out[0:n:2] = dump[1:n + 1:2]
    out[1:n:2] = dump[0:n:2]
    return bytes(out)


def parse(path: Path) -> tuple[list[int], list[tuple[int, int, int]]]:
    addrs: list[int] = []
    tables: list[tuple[int, int, int]] = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split()
        if f[0] == "table" and len(f) == 4:
            tables.append((int(f[1], 16), int(f[2], 10), int(f[3], 10)))
        elif len(f) == 1:
            addrs.append(int(f[0], 16))
        else:
            raise ValueError(f"{path}: cannot read line {raw!r}")
    return addrs, tables


def expand(img: bytes, path: Path) -> list[int]:
    addrs, tables = parse(path)
    out = list(addrs)
    for at, count, stride in tables:
        if at + stride * (count - 1) + 4 > len(img):
            raise ValueError(f"{path}: table at 0x{at:06x} runs past the ROM")
        for k in range(count):
            off = at + stride * k
            out.append(int.from_bytes(img[off:off + 4], "big"))
    return sorted(out)


def pointer_offsets(img: bytes, want: set[int]) -> dict[int, list[int]]:
    """Every even ROM offset holding one of `want` as a 32-bit big-endian value."""
    where: dict[int, list[int]] = {}
    for off in range(0, len(img) - 3, 2):
        v = int.from_bytes(img[off:off + 4], "big")
        if v in want:
            where.setdefault(v, []).append(off)
    return where


def run_at(vals: list[int], i: int, where: dict[int, list[int]]) -> tuple[int, int, int]:
    """Longest stretch vals[i], vals[i+1], ... at one ROM offset and one even stride up to MAX_STRIDE:
    (length, offset, stride)."""
    best = (1, -1, 0)
    for off in where.get(vals[i], ()):
        for stride in range(2, MAX_STRIDE + 1, 2):
            k = 1
            while i + k < len(vals) and off + stride * k in where.get(vals[i + k], ()):
                k += 1
            if k > best[0]:
                best = (k, off, stride)
    return best


def pack(img: bytes, vals: list[int]) -> tuple[list[int], list[tuple[int, int, int]]]:
    """Greedy, left to right: every stretch of >= 2 consecutive list values found in the ROM at one
    stride becomes a table line; repeat on what is left until no stretch remains."""
    rest = sorted(vals)
    where = pointer_offsets(img, set(rest))
    tables: list[tuple[int, int, int]] = []
    while True:
        i, keep, found = 0, [], False
        while i < len(rest):
            k, off, stride = run_at(rest, i, where)
            if k >= 2:
                tables.append((off, k, stride))
                found = True
            else:
                keep.append(rest[i])
            i += k
        rest = keep
        if not found:
            return rest, sorted(tables)


def longest_stretch(img: bytes, dump: bytes, vals: list[int]) -> tuple[int, str]:
    """Longest run of consecutive list values found in the ROM in list order: 32-bit big-endian in CPU
    order at any even stride up to MAX_STRIDE, or contiguous as 24/32-bit, either endianness, either
    byte order of the dump."""
    vals = sorted(vals)
    where = pointer_offsets(img, set(vals))
    best, desc = 1, "none"
    for i in range(len(vals) - 1):
        k, off, stride = run_at(vals, i, where)
        if k > best:
            best, desc = k, f"0x{vals[i]:06x}.. at ROM 0x{off:06x}, stride {stride}"
    for name, image in (("CPU order", img), ("as dumped", dump)):
        for width in (3, 4):
            for order in ("big", "little"):
                for i in range(len(vals) - 1):
                    k = 1
                    while i + k < len(vals):
                        pat = b"".join(x.to_bytes(width, order) for x in vals[i:i + k + 1])
                        if pat not in image:
                            break
                        k += 1
                    if k > best:
                        best, desc = k, f"0x{vals[i]:06x}.. {8 * width}-bit {order}-endian, {name}"
    return best, desc


def write_hex(path: Path, vals: list[int]) -> None:
    path.write_bytes("".join(f"{v:06x}\n" for v in vals).encode())


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[0] not in ("expand", "pack", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    cmd, dump = argv[0], Path(argv[1]).read_bytes()
    img = cpu_image(dump)
    src = Path(argv[2])
    out = Path(argv[argv.index("-o") + 1]) if "-o" in argv else None
    try:
        if cmd == "expand":
            if not out:
                raise ValueError("expand needs -o <out.txt>")
            write_hex(out, expand(img, src))
            return 0
        if cmd == "pack":
            if not out:
                raise ValueError("pack needs -o <list.txt>")
            rest, tables = pack(img, expand(img, src))
            body = (HEADER + "".join(f"table {at:06x} {n} {st}\n" for at, n, st in tables)
                    + "".join(f"{v:06x}\n" for v in rest))
            out.write_bytes(body.encode())
            print(f"{src.name}: {len(rest)} addresses + {len(tables)} table lines "
                  f"({sum(t[1] for t in tables)} pointers)")
            return 0
        addrs, _ = parse(src)
        n, desc = longest_stretch(img, dump, addrs)
        print(f"{src.name}: {len(addrs)} addresses; longest stretch in the ROM in list order: {n} ({desc})")
        return 1 if n >= 2 else 0
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
