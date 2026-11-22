# mpc-korg-prophecy

**Prophetic** (by KP Sim): a native port of the Korg Prophecy's synthesis to an Akai MPC
(Live III / XL, 64-bit) instrument plugin. Nothing is emulated on the MPC. The firmware's DSP programs are translated to C++, and the
control layer is reimplemented natively.

**Bring your own ROM.** This repo contains no Korg firmware and no data derived from it. You
supply your own dumps, and the build extracts and compiles what it needs from them on your machine.
See [docs/BYO-ROM.md](docs/BYO-ROM.md).

Not affiliated with or endorsed by Korg or inMusic.

## Make the plugin (ROM owners)

| Step | What to do |
|---|---|
| 0. Source | Download `kp-sim-prophetic-<version>.zip` from this repository's **Releases** page and unzip it. GitHub's green "Code > Download ZIP" and the release's "Source code" links leave out the MPC SDK (a git submodule). With git: `git clone --recursive` |
| 1. Python | Python 3.9 or newer. Windows: [python.org](https://www.python.org/downloads/) (tick "Add python.exe to PATH") or `winget install Python.Python.3.12`. macOS and Linux usually have `python3` |
| 2. Your ROMs | Put your Prophecy **v2.0** dumps of ic12 and ic22 (512 KiB each) in `roms\`. Any file names, loose or in a `.zip`; byte-swapped dumps are put right. Optional: your `.syx` files (they become user programs) |
| 3. Make it | Windows: double-click `Make Plugin.bat`. macOS / Linux: `python3 tools/make_plugin.py`. The first run downloads the compiler (zig, via the MPC SDK; needs internet once). No Visual Studio, no emulator |
| 4. Install | Copy `out\plugin\KP Sim - VST - Prophetic\` into `VST2 Plugins\` at the top of a USB stick, SD card or the MPC's drive. MPC OS 3.10.1+, Preferences > Enhancements (Beta) > Enable Enhancements + External Plugin Support, then restart the MPC |

When the dumps are wrong, the tool says which chip is missing, which file is a bad read, or that a
v1.7 / v1.01 set was found (not supported yet: the plugin is made from v2.0). The plugin folder holds
data derived from your ROMs: keep it on your own devices, do not share it.

## Layout

| Path | Contents | Tracked |
|---|---|---|
| `roms.json` | Expected firmware sets: names, sizes, CRC32/SHA-1 (no content) | yes |
| `tools/check_roms.py` | Identify dumps by hash; `--install` copies them to `data/roms/<set>/` | yes |
| `tools/audit_no_rom.py` | Fails if any tracked file contains ROM bytes or transcribed ROM tables | yes |
| `tools/make_plugin.py`, `Make Plugin.bat` | The whole build from your dumps in `roms/` to `out/plugin/` (docs/BYO-ROM.md, "End-user pipeline") | yes |
| `tools/make_release.py` | Source release zip in `out/release/` (HEAD + SDK, audited, no `reference/`) | yes |
| `roms/` | Your dumps for `make_plugin` (only its README is tracked) | **no** |
| `reference/wsl/` | Fetch/build/run the MAME-based reference oracle (dev only, never shipped) | yes |
| `reference/patches/` | Tracing patches applied to the reference emulator | yes |
| `docs/` | Plan, BYO-ROM model, release checklist and catalog entry (RELEASE.md) | yes |
| `data/roms/<set>/` | Your firmware dumps | **no** |
| `data/traces/` | Oracle captures: DSP program loads, coefficient writes, audio | **no** |
| `generated/` | C++ emitted from your ROMs at build time | **no** |

## Firmware sets

| Set | Version | ic12 (V55PI) | ic22 (H8/3003) | Status |
|---|---|---|---|---|
| `korgpro20` | v2.0 | 512 KiB | 512 KiB | primary target |
| `korgprop` | v1.7 | 512 KiB | 128 KiB | Profligacy's hardware-validated set |
| `korgpro101` | v1.01 | 512 KiB | 128 KiB | listed only |

```
py tools/check_roms.py "<folder with your dumps>" --install
```

## Windows test build

| Step | Command |
|---|---|
| Build | `powershell -File apps\win\build.ps1` (VS 2022 C++ tools, Python) -> `out\win\prophecy_win.exe`; the first build also generates everything firmware-derived from your ROMs in `out\win\data`: the H8 control code (`tools/h8dec.py`, `tools/h8xlat.py`), the engine snapshot (native boot) and the compiled DSP programs (`-Regen` to redo, see [docs/TRANSLATOR.md](docs/TRANSLATOR.md)); no emulator |
| Data | `out\win\data\`: `ic22.bin` and `ic12.bin` (your ROMs; ic12 supplies the 128 factory programs, the oscillator-model defaults, the Performance Editor curve and the arpeggio patterns), `state.bin` + `ram.bin` (engine snapshot: without them the engine boots the firmware natively from your ROMs, the H8's reset handler plus the V55's power-on messages, and writes them; the first Windows build does this, so does the app or the plugin on its first load; see docs/H8.md, "Native boot"); `programs\NNN.uart/.name` (captured bursts) are only used without ic12.bin; `User\*.syx`: your SysEx libraries |
| Play | run `out\win\prophecy_win.exe`; click a program; PC keys Z..M / Q..I (Left/Right = octave) or a MIDI keyboard |
| Edit | right-hand list: the Performance Editor (PERF: Knob 1-5, PE1-4) and every program parameter with its value; select one and move the slider below it. MIDI CC19-23 are the PE knobs |
| Arpeggiator | F1 on/off, F2 pattern (Up/Down/Alt1/Alt2/Random/Pat1-5), F3 octaves, F4 latch, F5 step, F6/F7 tempo |
| Offline check | `prophecy_win.exe --render out.wav <program> <note>` (1 s idle, 1 s note, 2 s release; writes `out.wav.txt` with engine load) |

## Status

See [docs/PLAN.md](docs/PLAN.md).

## MPC plugin (Live III / XL)

| Step | Command / action |
|---|---|
| Build | `py tools/stage_mpc.py` (stages `out/mpc/Prophecy` from the engine, generated H8 code, the compiled DSP programs in `generated/dspaot` (from the Windows build or `make -C engine dspaot`) and your local data, then runs the SDK's `build.py`) |
| Output | `out\mpc\Prophecy\build\KP Sim - VST - Prophetic\` (arm64 + arm32 `.so`, skins, `Content/` with your ROM-derived data) |
| Install | copy that folder into `VST2 Plugins\` at the root of a USB stick / SD card / the MPC's drive |
| Enable | MPC OS 3.10.1+, Preferences -> Enhancements (Beta) -> Enable Enhancements + External Plugin Support |
| Use | External Plugins -> KP Sim Prophetic; presets = the 128 factory programs, then your own; MIDI CC/bend/aftertouch work |
| Define | DEFINE page: Performance Editor Define (pick PE set and knob; set the parameter it edits, its left / right values and curve) and Arpeggio Pattern Define (pick a step of the selected pattern; set tone, velocity, gate, timing offset; arpeggio type and octave alternation); the wheel / ribbon / pedal Controller Assignment as automation; projects keep all of it |
| Pads | MIDI only reaches the plugin (no pad hardware access): pitch bend, channel and polyphonic aftertouch (pressure on the last key played) and CCs all work as on the Prophecy; MIDI In 1-3 on the DEFINE page show the last messages received (newest first) |
| Your programs | put Prophecy SysEx files (`.syx`: single program, bank, all programs, all data or arpeggio pattern dumps) in `Content\User\` of the plugin folder: their programs follow the factory ones as presets `U001 ...` (MIDI: bank select CC32 = 2, 3, ... + program change), their arpeggio patterns replace the ROM's |
| Edit | 525 program parameters (every parameter that reaches the sound engine), 454 of them on 32 pages (MAIN, OSC, PITCH EG, VOICE, one page per oscillator model, SHAPE, MIXER, FILTER, AMP, EG1-4, LFO1-4, effects); the rest are automatable. Edits reach the engine the way the Prophecy's panel edits do; projects save the program plus your edits. See [docs/PARAMS.md](docs/PARAMS.md) |
| Performance Editor | MAIN page: Perf Knob 1-5 and PE1-4 On, set by each program; a knob edits the parameters its switched-on PE sets are assigned to, computed as the Prophecy does (checked against the oracle); MIDI CC19-23 turn the knobs, NRPN 00 06-00 09 switch the sets |
| Arpeggiator | ARP/GLB page: On, Pattern (Up, Down, Alt1, Alt2, Random and the user patterns Pat1-5 from your ROM; selecting one loads its stored settings), Octave 1-4, Step 1/4..1/16T, Gate (% or Step), Velocity (Key, fixed or Step), Latch, Key Sync (Off by default, as on the Prophecy: the first step waits for the arpeggio clock), Sort, scan zone, BPM, Vel / Gate Int; follows the MPC's tempo and locks to its grid while the transport plays; note sequences match the Prophecy's V55 in 34 captured scenarios (`engine/src/arp.h`); NRPN 00 01-00 05 as on the Prophecy |
| Global | ARP/GLB page: Master Tune (430-450 Hz), Dly/Rev On (the Global Delay/Reverb bypass); as automation: User Scale 1 and 2, Knob1-5 Ctrl (the MIDI message that turns each PE knob, default CC19-23), Arp Vel / Gate Ctrl (the controller that scales arpeggio velocity / gate), Transpose Position; Transpose on MAIN |
| Voices | MAIN page: 1 (default) is the Prophecy, monophonic; 2-8 play chords with one complete Prophecy engine per voice (each bit-exact, with its own effects; the allocator takes the voice released longest ago, else steals the one held longest; pad pressure goes to the voice playing its pad). Each sounding voice costs one engine's CPU; silent voices sleep. Raising Voices loads the program with your edits, your global settings and the current controller positions into the voices added; the notes already playing go on undisturbed. Lowering it fades the voices above the new count out over 20 ms |
| Check | `Engine Load` (MIXER page) shows the engine's render time as % of real time; with MIDI Log on, `Content/midi_log.txt` gets a LOAD line every second (average, peak, idle sleep, CPU core) |

The built folder contains firmware-derived files: keep it on your own devices, do not share it.

## Licence

| Part | Licence |
|---|---|
| This project's own code and docs | GPL-3.0-or-later ([LICENSE](LICENSE)) |
| `engine/third_party/tms57002/` | BSD-3-Clause (from MAME; its LICENSE and PROVENANCE.md) |
| `extern/mpc-plugin-builder-sdk/` | MIT (inMusic Brands; its LICENSE) |
| Your ROMs and everything built from them | not part of this project: yours, keep them private |

Korg and Prophecy are trademarks of Korg Inc., used here only to say what the plugin models.
