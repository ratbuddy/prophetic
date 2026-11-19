#!/usr/bin/env python3
"""Fail if any git-tracked (or staged) file contains firmware-derived content.

Two checks against every ROM found under data/roms/, as dumped and word-swapped (the CPU's byte
order for a 16-bit-bus ROM dumped low byte first):
  1. raw bytes  - any copied run of >= 31 ROM bytes (16-byte windows indexed at stride 16)
  2. transcribed tables - runs of >= 8 integer literals in text files (decimal and 0x-hex, and
     separately every 2-8 digit token read as bare hex), re-encoded as 8/16/24/32-bit big- and
     little-endian bytes, then checked the same way

Run before every commit/push:  py tools/audit_no_rom.py
Exit 0 = clean, 1 = hits found, 2 = no ROMs available to audit against.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROM_ROOT = REPO / "data" / "roms"
W = 16          # window length
STRIDE = 16     # index stride -> detects copies of length >= W + STRIDE - 1
MIN_DISTINCT = 5  # skip low-entropy windows (padding, 0xFF fill, ramps of one byte)
MIN_LITERALS = 8
INT_RE = re.compile(r"(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)(?![\w.])")
HEX_RE = re.compile(r"(?<![\w.])([0-9a-fA-F]{2,8})(?![\w.])")   # bare hex: address lists, hex dumps
RUN_SEP_RE = re.compile(r"^[\s,{}\[\]()]*$")


def swap16(b: bytes) -> bytes:
    n = len(b) - (len(b) & 1)
    out = bytearray(n)
    out[0:n:2] = b[1:n + 1:2]
    out[1:n:2] = b[0:n:2]
    return bytes(out)


def build_index() -> dict[bytes, str]:
    index: dict[bytes, str] = {}
    for rom in sorted(ROM_ROOT.rglob("*.bin")):
        raw = rom.read_bytes()
        for view, data in (("", raw), (" word-swapped", swap16(raw))):
            for off in range(0, len(data) - W + 1, STRIDE):
                win = data[off:off + W]
                if len(set(win)) >= MIN_DISTINCT:
                    index.setdefault(win, f"{rom.relative_to(REPO)}{view}@0x{off:06x}")
    return index


def scan_bytes(blob: bytes, index: dict[bytes, str]) -> str | None:
    for off in range(0, len(blob) - W + 1):
        hit = index.get(blob[off:off + W])
        if hit:
            return f"offset {off} matches {hit}"
    return None


def literal_runs(text: str, bare_hex: bool = False):
    """Yield runs of consecutive integer literals separated only by punctuation/whitespace
    (decimal and 0x-hex; with bare_hex, every 2-8 digit hex token)."""
    run: list[int] = []
    last_end = 0
    for m in (HEX_RE if bare_hex else INT_RE).finditer(text):
        if run and not RUN_SEP_RE.match(text[last_end:m.start()]):
            if len(run) >= MIN_LITERALS:
                yield run
            run = []
        tok = m.group(1)
        run.append(int(tok, 16) if bare_hex or tok[:2] in ("0x", "0X") else int(tok, 10))
        last_end = m.end()
    if len(run) >= MIN_LITERALS:
        yield run


def encodings(values: list[int]):
    for width in (1, 2, 3, 4):
        mask = (1 << (8 * width)) - 1
        if any(v > mask for v in values):
            continue
        for order in ("big", "little"):
            yield width, order, b"".join((v & mask).to_bytes(width, order) for v in values)


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         cwd=REPO, capture_output=True, check=True).stdout
    return [REPO / p for p in out.decode().split("\0") if p]


def main() -> int:
    index = build_index()
    if not index:
        print("audit: no ROMs under data/roms/ - cannot audit", file=sys.stderr)
        return 2
    hits = []
    for path in tracked_files():
        if not path.is_file():
            continue
        blob = path.read_bytes()
        rel = path.relative_to(REPO)
        hit = scan_bytes(blob, index)
        if hit:
            hits.append(f"{rel}: raw bytes, {hit}")
            continue
        try:
            text = blob.decode("utf-8")
        except UnicodeDecodeError:
            continue
        def table_hit() -> str | None:
            for bare_hex in (False, True):
                for run in literal_runs(text, bare_hex):
                    for width, order, enc in encodings(run):
                        hit = scan_bytes(enc, index)
                        if hit:
                            kind = "bare-hex" if bare_hex else "literal"
                            return f"{width * 8}-bit {order}-endian {kind} table, {hit}"
            return None
        hit = table_hit()
        if hit:
            hits.append(f"{rel}: {hit}")
    for h in hits:
        print("ROM CONTENT:", h)
    print(f"audit: {len(hits)} hit(s) across tracked files, index={len(index)} windows")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
