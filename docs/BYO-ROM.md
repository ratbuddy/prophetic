# Bring-your-own-ROM model

Goal: the distributable artifact is this repo, meaning our own source code plus scripts. Everything
derived from Korg firmware is produced on the user's machine from the user's own dumps and is never
committed or distributed. This reduces exposure; it is not a legal opinion.

## What lives where

| Layer | In repo (ours) | Produced locally from user ROMs |
|---|---|---|
| DSP engine | Generic TMS57002 to C++ translator, emitting fixed-point code with the semantics of the documented instruction set | The 16 Prophecy DSP programs (12 oscillator, 1 filter, 2 effects, 1 clear) and their initial coefficient memory |
| Control layer | Native C++ for voice/param/modulation logic, with every firmware constant read through a table interface | Lookup tables and curves, read from the ROM at offsets listed in a per-version map |
| Presets | Program-record parser, parameter metadata (names, ranges), the V55's program-change burst rules, a SysEx dump reader | The 128 factory programs (read from your ic12 at load), your own SysEx libraries |
| Plugin | MPC SDK wrapper, UI skins, build scripts | The compiled `.so` (the user builds it for their own device) |

## Rules

| # | Rule | Enforced by |
|---|---|---|
| 1 | No ROM bytes and no transcribed ROM tables in tracked files | `tools/audit_no_rom.py` (raw bytes plus 8/16/24/32-bit literal tables) |
| 2 | `data/`, `generated/`, `build/` are never committed | `.gitignore` |
| 3 | Per-version offset maps hold addresses and lengths only, never values | review plus audit |
| 4 | Firmware constants enter C++ only through the generated table header | code review |
| 5 | The end-user build needs ROMs plus a compiler, not the emulator | `tools/make_plugin.py` (and, for the Windows test app, `apps/win/build.ps1`) from the ROMs alone: the H8 code from `tools/h8dec.py` (our H8/300H decoder) and `tools/h8xlat.py`, the engine snapshot (`state.bin`, `ram.bin`) from a native boot (docs/H8.md, "Native boot"), the DSP programs from `dsp_aot_gen` |
| 6 | The reference emulator is a dev oracle for finding offsets and verifying output | `reference/` is dev-only |

## End-user pipeline

`tools/make_plugin.py` (Windows: `Make Plugin.bat`) from the dumps in `roms/` alone; needs Python 3.9+
and the zig compiler the MPC SDK installs on first use.

| Step | Does | Output (all gitignored) |
|---|---|---|
| 1 | Finds ic12 + ic22 by SHA-1 (`roms.json`): any names, inside `.zip`, byte-swapped put right; reports missing chips, bad reads, unsupported versions | `out/make_plugin/data/` |
| 2 | Translates the H8 code: `tools/h8_addrlist.py` expands `romdata/<set>_h8_*.txt` (their pointer-table lines read from your ic22), then `tools/h8dec.py`, `tools/h8xlat.py` | `generated/h8ctl/h8_gen.cpp` |
| 3 | Builds the DSP program compiler for the host (zig c++) | `out/make_plugin/dsp_aot_gen` |
| 4 | Boots the firmware natively (engine snapshot), compiles the DSP programs | `state.bin`, `ram.bin`, `generated/dspaot/dsp_aot_gen.inc` |
| 5 | `tools/stage_mpc.py --arm64-cpu cortex_a76`: SDK build, arm64 + arm32 | `out/mpc/Prophecy/` |
| 6 | Copies the plugin folder | `out/plugin/KP Sim - VST - Prophetic/` |

The source release is `tools/make_release.py`: the HEAD commit plus the SDK submodule (git archive),
without `reference/`, only after `tools/audit_no_rom.py` reports `audit: 0 hit`.
