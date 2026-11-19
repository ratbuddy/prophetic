#!/usr/bin/env python3
"""Package the source release: what a ROM owner needs to make the plugin, nothing derived from firmware.

Usage: py tools/make_release.py [--allow-dirty]

Writes out/release/kp-sim-<name>-<version>.zip (name and version from plugins/Prophecy/plugin.json): the
files of the HEAD commit of this repository and of the MPC SDK submodule (git archive), without the
developer-only reference emulator scripts (reference/). Users unzip it, put their dumps in roms/ and
run "Make Plugin.bat" or tools/make_plugin.py.

Refuses when tools/audit_no_rom.py does not report "audit: 0 hit" (it needs your ROMs in data/roms/
to compare against) and when there are uncommitted changes (the zip holds HEAD; --allow-dirty to
package HEAD anyway).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDK_REL = "extern/mpc-plugin-builder-sdk"
EXCLUDE = ["reference", ".gitmodules"]   # dev-only: the MAME-based oracle's fetch/patch scripts
OUT = REPO / "out" / "release"


def git(*args: str, cwd: Path = REPO) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allow-dirty", action="store_true")
    args = ap.parse_args()

    dirty = git("status", "--porcelain").strip()
    if dirty and not args.allow_dirty:
        print("uncommitted changes (the release holds the HEAD commit); commit first or pass --allow-dirty:\n" + dirty,
              file=sys.stderr)
        return 1
    audit = subprocess.run([sys.executable, str(REPO / "tools" / "audit_no_rom.py")], cwd=REPO,
                           capture_output=True, text=True)
    sys.stdout.write(audit.stdout)
    if audit.returncode != 0 or "audit: 0 hit" not in audit.stdout:
        sys.stderr.write(audit.stderr)
        print("the ROM audit did not pass: no release", file=sys.stderr)
        return 1

    manifest = json.loads((REPO / "plugins" / "Prophecy" / "plugin.json").read_text(encoding="utf-8"))
    name = f"kp-sim-{manifest['name'].lower()}-{manifest['version']}"
    OUT.mkdir(parents=True, exist_ok=True)
    main_zip, sdk_zip, final = OUT / "_repo.zip", OUT / "_sdk.zip", OUT / f"{name}.zip"
    git("archive", "--format=zip", f"--prefix={name}/", "-o", str(main_zip), "HEAD", "--", ".",
        *[f":(exclude){p}" for p in EXCLUDE + [SDK_REL]])
    git("archive", "--format=zip", f"--prefix={name}/{SDK_REL}/", "-o", str(sdk_zip), "HEAD", cwd=REPO / SDK_REL)

    count = 0
    with zipfile.ZipFile(final, "w", zipfile.ZIP_DEFLATED) as out:
        for part in (main_zip, sdk_zip):
            with zipfile.ZipFile(part) as z:
                for info in z.infolist():
                    out.writestr(info, z.read(info))
                    count += not info.is_dir()
            part.unlink()
    commit = git("rev-parse", "--short", "HEAD").strip()
    sdk_commit = git("rev-parse", "--short", "HEAD", cwd=REPO / SDK_REL).strip()
    print(f"wrote {final} ({count} files, {final.stat().st_size // 1024} KiB; commit {commit}, SDK {sdk_commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
