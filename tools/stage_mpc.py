#!/usr/bin/env python3
"""Stage the MPC plugin as a self-contained SDK project and build it with the SDK's build.py.

Usage: py tools/stage_mpc.py [--data out/win/data] [--no-build] [--arm64-cpu cortex_a76] [--opt 3] [build.py args...]

Creates out/mpc/Prophecy/ (gitignored):
  plugin.json, ui.json, README.md      from plugins/Prophecy/
  src/*                                 plugin source + engine + vendored TMS57002 core + shim +
                                        generated H8 control code (generated/h8ctl/h8_gen.cpp) +
                                        compiled DSP programs (generated/dspaot/dsp_aot_gen.inc) +
                                        tms57002.hxx (tmsmake), flattened with include paths rewritten.
                                        The vendored core .cpp files are staged as .inl: they are
                                        compiled once, inside dsp_core.cpp (one translation unit)
  Content/                              your locally generated, ROM-derived data (--data)
then runs extern/mpc-plugin-builder-sdk/tools/build.py on it. The staged project and the plugin it
builds contain firmware-derived files: keep them for your own device, do not distribute.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDK = REPO / "extern" / "mpc-plugin-builder-sdk"
STAGE = REPO / "out" / "mpc" / "Prophecy"

SOURCES = [
    "plugins/Prophecy/src/Prophecy.cpp",
    "engine/src/synth.h", "engine/src/synth.cpp", "engine/src/poly.h", "engine/src/poly.cpp", "engine/src/arp.h", "engine/src/arp.cpp",
    "engine/src/prophecy_board.h", "engine/src/prophecy_board.cpp",
    "engine/src/control/h8ctl.h", "engine/src/control/h8ctl.cpp",
    "engine/src/control/h8rt.h", "engine/src/control/rom_view.h",
    "engine/src/control/params.h", "engine/src/control/params.cpp", "engine/src/control/params_table.inc",
    "engine/src/control/perf.h", "engine/src/control/perf.cpp",
    "engine/src/control/sysex.h", "engine/src/control/sysex.cpp",
    "engine/src/dsp_core.cpp", "engine/src/dsp_fast.h",
    "engine/mame_shim/emu.h", "engine/mame_shim/57002dsm.h", "engine/mame_shim/tms57002_jit.h",
    "engine/third_party/tms57002/tms57002.cpp", "engine/third_party/tms57002/tms57002.h",
    "engine/third_party/tms57002/tms57kdec.cpp", "engine/third_party/tms57002/tmsops.cpp",
    "generated/h8ctl/h8_gen.cpp",
]
OPTIONAL = ["generated/dspaot/dsp_aot_gen.inc"]   # without it the plugin runs the DSP interpreter (slower)
CORE_CPP = {"tms57002.cpp", "tms57kdec.cpp", "tmsops.cpp"}   # staged as .inl, included by dsp_core.cpp
INCLUDE_RE = re.compile(r'#include\s+"(?:control/|cpu/tms57002/)([^"]+)"')
CORE_INCLUDE_RE = re.compile(r'#include\s+"(tms57002|tms57kdec|tmsops)\.cpp"')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=str(REPO / "out" / "win" / "data"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--arm64-cpu", help="arm64 -mcpu (SDK default cortex_a55; MPC Live III: cortex_a76)")
    ap.add_argument("--opt", help="optimization level (SDK default 2)")
    args, build_args = ap.parse_known_args()

    if STAGE.exists():
        shutil.rmtree(STAGE)
    src = STAGE / "src"
    src.mkdir(parents=True)
    for name in ("plugin.json", "ui.json", "README.md"):
        p = REPO / "plugins" / "Prophecy" / name
        if p.exists():
            shutil.copy2(p, STAGE / name)
    for rel in SOURCES + OPTIONAL:
        p = REPO / rel
        if not p.exists():
            if rel in OPTIONAL:
                print(f"note: {rel} not found (make -C engine dspaot): the plugin will run the DSP interpreter", file=sys.stderr)
                continue
            print(f"missing {rel}" + (" (run apps/win/build.ps1: it generates it from your ROMs)" if "generated" in rel else ""), file=sys.stderr)
            return 1
        text = INCLUDE_RE.sub(r'#include "\1"', p.read_text(encoding="utf-8"))
        text = CORE_INCLUDE_RE.sub(r'#include "\1.inl"', text)
        name = p.stem + ".inl" if p.name in CORE_CPP else p.name
        (src / name).write_text(text, encoding="utf-8")
    tp = REPO / "engine" / "third_party" / "tms57002"
    subprocess.run([sys.executable, str(tp / "tmsmake.py"), "s", str(tp / "tmsinstr.lst"), str(src / "tms57002.hxx")], check=True)
    subprocess.run([sys.executable, str(REPO / "tools" / "tms_regops.py"), str(src / "tms57002.hxx"), str(tp / "tms57002.cpp"),
                    str(tp / "tms57002.h"), str(src / "tms57002_regops.hxx")], check=True)

    data = Path(args.data)
    content = STAGE / "Content"
    for need in ("ic22.bin", "ic12.bin"):
        if not (data / need).exists():
            print(f"missing {data / need}: install your ROMs first (README: Windows test build)", file=sys.stderr)
            return 1
    if not ((data / "state.bin").exists() and (data / "ram.bin").exists()):
        print("note: no state.bin / ram.bin in the data: the plugin boots the firmware on its first load (a few "
              "seconds) and saves them in its Content folder; the Windows build writes them up front", file=sys.stderr)
    shutil.copytree(data, content)
    print(f"staged {STAGE} ({len(list(src.iterdir()))} source files, Content from {data})")
    if args.no_build:
        return 0
    if args.arm64_cpu or args.opt:   # the SDK's build with another arm64 CPU target / optimization level
        return subprocess.run([sys.executable, str(REPO / "tools" / "sdk_build_tuned.py"), str(SDK),
                               args.arm64_cpu or "cortex_a55", args.opt or "2", str(STAGE)] + build_args).returncode
    return subprocess.run([sys.executable, str(SDK / "tools" / "build.py"), str(STAGE)] + build_args).returncode


if __name__ == "__main__":
    sys.exit(main())
