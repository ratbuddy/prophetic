#!/usr/bin/env python3
"""Identify Prophecy firmware dumps by hash and optionally install them into data/roms/<set>/.

Usage:
  py tools/check_roms.py <dir>              # report which known sets are present in <dir>
  py tools/check_roms.py <dir> --install    # copy matching files to data/roms/<set>/<canonical name>

Files are matched by SHA-1 regardless of filename. Nothing is ever moved or deleted;
--install copies, and refuses to overwrite a differing file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "roms.json"
DEST_ROOT = REPO / "data" / "roms"
MAX_SIZE = 1 << 20


def sha1_of(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", type=Path)
    ap.add_argument("--install", action="store_true")
    args = ap.parse_args()

    sets = json.loads(MANIFEST.read_text())["sets"]
    by_sha1 = {f["sha1"]: (set_name, f) for set_name, s in sets.items() for f in s["files"]}

    found: dict[str, dict[str, Path]] = {}
    for path in sorted(p for p in args.dir.rglob("*") if p.is_file() and p.stat().st_size <= MAX_SIZE):
        hit = by_sha1.get(sha1_of(path))
        if hit:
            set_name, f = hit
            found.setdefault(set_name, {})[f["name"]] = path

    complete = []
    for set_name, s in sets.items():
        have = found.get(set_name, {})
        names = [f["name"] for f in s["files"]]
        status = "complete" if all(n in have for n in names) else ("partial" if have else "absent")
        print(f"{set_name:11} {s['label']:52} {status}")
        for n in names:
            print(f"    {'ok ' if n in have else '-- '} {n:15} {have.get(n, '')}")
        if status == "complete":
            complete.append(set_name)

    if not complete:
        print("no complete firmware set found", file=sys.stderr)
        return 1

    if args.install:
        for set_name in complete:
            dest_dir = DEST_ROOT / set_name
            dest_dir.mkdir(parents=True, exist_ok=True)
            for name, src in found[set_name].items():
                dest = dest_dir / name
                if dest.exists():
                    if sha1_of(dest) != sha1_of(src):
                        print(f"refusing to overwrite differing {dest}", file=sys.stderr)
                        return 2
                    continue
                shutil.copy2(src, dest)
                print(f"installed {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
