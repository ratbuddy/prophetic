#!/usr/bin/env python3
"""Make the KP Sim Prophetic plugin (the Korg Prophecy engine) for your MPC from your own Prophecy ROM dumps, in one step.

Usage:
  python tools/make_plugin.py [<folder or .zip with your dumps>] [--open]

Input (default: the roms/ folder at the top of this project): your dumps of the Prophecy v2.0 ROMs
ic12 (V55PI) and ic22 (H8/3003). Any file names, loose or inside .zip files; byte-swapped dumps are
put right. Optional: your own Prophecy SysEx files (.syx), which become the plugin's user programs.

Needs Python 3.9+ and, the first time, internet: the MPC SDK fetches its compiler (zig) by itself.
No Visual Studio, no emulator.

Output: out/plugin/KP Sim - VST - Prophetic/. Copy that folder into "VST2 Plugins" on a drive of your
MPC. It holds data derived from your ROMs: keep it on your own devices, do not share it.

Steps:
  1. find and check the dumps (SHA-1 against roms.json)
  2. translate the H8 control code (tools/h8dec.py, tools/h8xlat.py)
  3. build the DSP program compiler for this computer (zig)
  4. boot the firmware natively, compile the DSP programs (engine snapshot + generated/dspaot)
  5. build the plugin for the MPC (tools/stage_mpc.py: arm64 tuned for the Live III, arm32)
  6. copy it to out/plugin/
--open opens out/plugin/ in the file browser at the end (Windows, macOS).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDK = REPO / "extern" / "mpc-plugin-builder-sdk"
WORK = REPO / "out" / "make_plugin"
OUT = REPO / "out" / "plugin"
MAX_SIZE = 1 << 20
ROLE = {"V55PI": "ic12.bin", "H8/3003": "ic22.bin"}   # roms.json cpu -> the engine's file name
STEPS = 6


class Fail(Exception):
    pass


def say(msg: str = "") -> None:
    print(msg, flush=True)


def step(n: int, msg: str) -> None:
    say(f"\n[{n}/{STEPS}] {msg}")


def run(argv: list, what: str) -> None:
    r = subprocess.run([str(a) for a in argv])
    if r.returncode != 0:
        raise Fail(f"{what} failed (exit code {r.returncode}); the messages above say why")


# ---------------------------------------------------------------------------------------------
# 1. The dumps
# ---------------------------------------------------------------------------------------------

def candidates(root: Path):
    """(label, bytes) for every file of at most 1 MiB under root, members of .zip files included."""
    paths = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
    for p in paths:
        if p.suffix.lower() == ".zip":
            try:
                with zipfile.ZipFile(p) as z:
                    for info in z.infolist():
                        if not info.is_dir() and info.file_size <= MAX_SIZE:
                            yield f"{p.name}: {info.filename}", z.read(info)
            except (zipfile.BadZipFile, OSError) as e:
                say(f"    note: could not read {p} as a zip file ({e})")
        elif p.stat().st_size <= MAX_SIZE:
            yield str(p), p.read_bytes()


def swap16(b: bytes) -> bytes:
    s = bytearray(b)
    s[0::2] = b[1::2]
    s[1::2] = b[0::2]
    return bytes(s)


def find_roms(src: Path, data: Path) -> str:
    """Writes ic12.bin + ic22.bin of a supported set to data/; returns the set name."""
    sets = json.loads((REPO / "roms.json").read_text(encoding="utf-8"))["sets"]
    supported = [n for n in sets if (REPO / "romdata" / f"{n}_h8_seeds.txt").exists()]
    by_sha1 = {f["sha1"]: (n, f) for n, s in sets.items() for f in s["files"]}
    sizes = {f["size"] for s in sets.values() for f in s["files"]}

    found: dict[str, dict[str, tuple[bytes, str, bool]]] = {}
    unknown = []
    for label, b in candidates(src):
        for fixed, swapped in ((b, False), (swap16(b), True)) if len(b) % 2 == 0 else ((b, False),):
            hit = by_sha1.get(hashlib.sha1(fixed).hexdigest())
            if hit:
                name, f = hit
                found.setdefault(name, {})[ROLE[f["cpu"]]] = (fixed, label, swapped)
                break
        else:
            if len(b) in sizes:
                unknown.append(label)

    for name in supported:
        have = found.get(name, {})
        if len(have) == len(sets[name]["files"]):
            for role, (b, label, swapped) in sorted(have.items()):
                (data / role).write_bytes(b)
                say(f"    {role[:-4]}: {label}" + ("  (byte-swapped dump, put right)" if swapped else ""))
            return name

    lines = []
    for name, have in found.items():
        missing = [ROLE[f["cpu"]][:-4] for f in sets[name]["files"] if ROLE[f["cpu"]] not in have]
        state = "complete" if not missing else "missing " + " and ".join(missing)
        lines.append(f"  found {sets[name]['label']}: {state}")
    for name in found:
        if name not in supported:
            lines.append(f"  {sets[name]['label']} is not supported yet: this plugin is made from the v2.0 ROMs")
    for name in supported:
        for f in sets[name]["files"]:
            if ROLE[f["cpu"]] not in found.get(name, {}):
                lines.append(f"  needed: the {sets[name]['label']} {ROLE[f['cpu']][:-4]} ({f['cpu']}), "
                             f"{f['size'] // 1024} KiB, SHA-1 {f['sha1']}")
    if unknown:
        lines.append("  these files have a Prophecy ROM's size but match no known dump (a bad read, or a version "
                     "this tool does not know): " + ", ".join(unknown))
    raise Fail(f"no complete set of supported Prophecy dumps in {src}\n" + "\n".join(lines))


# ---------------------------------------------------------------------------------------------
# 2-4. Code and data generated from the dumps
# ---------------------------------------------------------------------------------------------

def sdk_build_module():
    """The MPC SDK's tools/build.py as a module (for its zig lookup and one-time install)."""
    spec = importlib.util.spec_from_file_location("sdk_build", SDK / "tools" / "build.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sdk_build"] = mod
    spec.loader.exec_module(mod)
    return mod


def build_aot_compiler(gen: Path) -> Path:
    tp = REPO / "engine" / "third_party" / "tms57002"
    hxx = gen / "cpu" / "tms57002"
    hxx.mkdir(parents=True, exist_ok=True)
    run([sys.executable, tp / "tmsmake.py", "s", tp / "tmsinstr.lst", hxx / "tms57002.hxx"], "tmsmake")
    run([sys.executable, REPO / "tools" / "tms_regops.py", hxx / "tms57002.hxx", tp / "tms57002.cpp",
         tp / "tms57002.h", hxx / "tms57002_regops.hxx"], "tms_regops")

    sdk = sdk_build_module()
    try:
        zig = sdk.find_zig()
    except sdk.BuildError as e:
        raise Fail(f"no compiler: {e}")
    e = REPO / "engine"
    sources = [e / "src" / "dsp_core.cpp", e / "tools" / "dsp_aot_gen.cpp", e / "src" / "prophecy_board.cpp",
               e / "src" / "control" / "h8ctl.cpp", e / "src" / "control" / "params.cpp",
               e / "src" / "control" / "perf.cpp", e / "src" / "control" / "sysex.cpp", e / "src" / "synth.cpp",
               e / "src" / "arp.cpp", REPO / "generated" / "h8ctl" / "h8_gen.cpp"]
    inc = [e / "mame_shim", e / "third_party" / "tms57002", gen, e / "src", REPO / "generated" / "dspaot"]
    exe = WORK / ("dsp_aot_gen.exe" if os.name == "nt" else "dsp_aot_gen")
    run(zig + ["c++"] + sdk.native_target_flags_for_zig() +
        ["-std=c++17", "-O2", "-w", "-DKP_DSP_AOT_NONE", "-DNOMINMAX", "-D_CRT_SECURE_NO_WARNINGS"] +
        [f"-I{p}" for p in inc] + sources + ["-o", exe], "compiling the DSP program compiler")
    return exe


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roms", nargs="?", default=str(REPO / "roms"), help="folder or .zip with your dumps")
    ap.add_argument("--open", action="store_true", help="open out/plugin/ in the file browser when done")
    args = ap.parse_args()
    if sys.version_info < (3, 9):
        raise Fail("Python 3.9 or newer is needed")
    if not (SDK / "tools" / "build.py").exists():
        raise Fail(f"the MPC SDK is missing in {SDK}: download kp-sim-prophetic-<version>.zip from the "
                   "repository's Releases page (GitHub's own \"Download ZIP\" and \"Source code\" leave the SDK out), "
                   "or in a git clone run: git submodule update --init")
    src = Path(args.roms)
    if not src.exists():
        raise Fail(f"{src} does not exist: put your ROM dumps there, or name the folder or .zip they are in")
    t0 = time.time()

    step(1, f"Your ROM dumps ({src})")
    if WORK.exists():
        shutil.rmtree(WORK)
    data = WORK / "data"
    data.mkdir(parents=True)
    rom_set = find_roms(src, data)
    syx = sorted(p for p in ([src] if src.is_file() else src.rglob("*")) if p.is_file() and p.suffix.lower() == ".syx")
    if syx:
        (data / "User").mkdir()
        for p in syx:
            shutil.copy2(p, data / "User" / p.name)
        say(f"    your SysEx files: {len(syx)} (user programs and arpeggio patterns)")

    step(2, "Translating the H8 control code")
    h8 = REPO / "generated" / "h8ctl"
    h8.mkdir(parents=True, exist_ok=True)
    ir = WORK / "ic22.ir"
    lists = {}
    for kind in ("seeds", "entries"):   # address lists: their pointer-table lines are read from your ic22
        lists[kind] = WORK / f"h8_{kind}.txt"
        run([sys.executable, REPO / "tools" / "h8_addrlist.py", "expand", data / "ic22.bin",
             REPO / "romdata" / f"{rom_set}_h8_{kind}.txt", "-o", lists[kind]], "h8_addrlist")
    run([sys.executable, REPO / "tools" / "h8dec.py", data / "ic22.bin", "--entries", lists["seeds"],
         "-o", ir], "h8dec")
    run([sys.executable, REPO / "tools" / "h8xlat.py", ir, h8 / "h8_gen.cpp", lists["entries"]], "h8xlat")

    step(3, "Building the DSP program compiler for this computer (the first time also installs zig)")
    exe = build_aot_compiler(WORK / "gen")

    step(4, "Booting the firmware natively and compiling the DSP programs")
    aot = REPO / "generated" / "dspaot" / "dsp_aot_gen.inc"
    aot.parent.mkdir(parents=True, exist_ok=True)
    run([exe, data, aot], "the DSP program compiler")
    for need in ("state.bin", "ram.bin"):
        if not (data / need).exists():
            raise Fail(f"the native boot wrote no {need}")

    step(5, "Building the plugin for the MPC")
    run([sys.executable, REPO / "tools" / "stage_mpc.py", "--data", data, "--arm64-cpu", "cortex_a76"],
        "the plugin build")

    step(6, "Collecting the plugin")
    built = sorted((REPO / "out" / "mpc" / "Prophecy" / "build").glob("* - VST - *"))
    if len(built) != 1:
        raise Fail(f"expected one plugin folder in out/mpc/Prophecy/build, found {len(built)}")
    dest = OUT / built[0].name
    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(built[0], dest)

    mins = (time.time() - t0) / 60
    say(f"""
Done in {mins:.1f} min. Your plugin:
    {dest}

To install it:
  1. Copy that whole folder into a folder named "VST2 Plugins" at the top of a USB stick, an SD card
     or the MPC's internal drive.
  2. On the MPC (MPC OS 3.10.1 or later): Preferences > Enhancements (Beta) > turn on Enable
     Enhancements and External Plugin Support.
  3. Restart the MPC: it looks for plugins when it starts. The plugin is "KP Sim Prophetic" under
     External Plugins.
To add your own programs later, put .syx files in the plugin's Content/User folder.
The folder holds data derived from your ROMs: keep it on your own devices, do not share it.""")
    if args.open:
        if os.name == "nt":
            os.startfile(OUT)
        elif sys.platform == "darwin":
            subprocess.run(["open", str(OUT)])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        say(f"\nERROR: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        say("\nstopped")
        sys.exit(1)
