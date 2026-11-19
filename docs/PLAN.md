# Plan

## Architecture facts (verified)

| Fact | Source |
|---|---|
| Chain: DSP1 oscillator, then DSP2 filter, then DSP3 effects/DAC; DSP2 SO1 feeds DSP1 SI0 with a 1-sample delay | `korgprophecy.cpp` (mame-profligacy) ~L4227 |
| DSP1 and DSP3 each have external DRAM (2x LH64256); DSP2 has none | `korgprophecy.cpp` ~L3971 |
| A program is up to 256 x 24-bit words, run from PC0 until `idle` once per sample frame | Profligacy README, `tmsinstr.lst` |
| Control flow exists but is static: `b`, `bgz`, `blz`, `bnz`, `bv`, `bioz` (immediate targets), `rptk` repeat. Translation emits labels/goto per PC | `tmsinstr.lst` (opcode type 3: 48/50/58/60/70/78, `rptk` 10) |
| Translator strategy: specialise the BSD-3 MAME core per PC (decoded fields constant), preserving the MAC/MPY pipeline (`macc_write`/read split) | `tmsinstr.lst` semantics, `tms57002.cpp` |
| Host interface: PLOAD alone = 24-bit words (ST0, ST1, then program); PLOAD+CLOAD = 32-bit CMEM init; CLOAD alone = SA byte + 32-bit queued coefficient update | `tms57002.cpp` `data_w` |
| ic12 = V55PI (MAME `maincpu`: panel/LCD/MIDI); ic22 = H8/3003 (MAME `subcpu`: DSP loads) | `korgprophecy.cpp` L434-435, ROM_START |
| User dumps = `korgpro20` (v2.0), SHA-1 verified | `roms.json` |
| All Prophecy sets flagged `MACHINE_NOT_WORKING` in the fork; Profligacy's hardware validation used v1.7 | `korgprophecy.cpp` L4373-4375 |
| MPC host: 44.1 kHz fixed, blocks 32-512, in-process, glibc 2.39, static libc++, aarch64 baseline cortex-a55 | mpc-plugin-builder-sdk `docs/host-contract.md`, `docs/building.md` |

## Findings (oracle, `korgpro20`, A00 boot program, note 60 at 11-13 s)

Reproduce: `reference/wsl/smoke.sh korgpro20 60 16`, then `py tools/trace_summary.py data/traces/smoke_korgpro20_n60 --dump`.

| Finding | Evidence |
|---|---|
| Oracle runs at 2.3x realtime (16 s emulated in 6.9 s, interpreter, Ryzen 9800X3D/WSL2) | `time` of `smoke.sh` |
| Audio: silent through boot (0-11 s), note audible 11-15 s, peak about -23 dBFS | `tools/wav_stats.py` |
| **Envelopes/modulation run on the H8**: per-voice coefficients stream at a fixed **228.6 Hz** (every 210 frames @ 48 kHz) on DSP1/2/3 while a note sounds, about 0 otherwise | `U` rows: median interval 210 frames, busiest addresses DSP1 30/34/37/38/D7/D9/DA/E2/E6/EB, DSP2 73/C3/C6/CC/CF/F0, DSP3 D0/E3 |
| Six distinct programs seen at boot: DSP1 x3 oscillator plus clear, DSP2 x1 filter, DSP3 x1 effects plus clear | `trace_summary.py` SHA-1s |
| All decoded opcodes are in MAME's table; branches only forward (DAG, no loops); `rptk` unused | `tools/tms_classify.py` |
| DSP1 set `47b6b926` and DSP3 effects use external DRAM (`rde`/`wre`) | classifier |
| Clear program `6c994577` (255 words, 42 x `wre`) is shared by DSP1 and DSP3 | classifier, SHA-1 |
| DSP3 effects program has no `idle`: it runs 256 words and stops at the PC wrap | classifier `idle@None` |
| **Complete DSP program set = 16**: 12 DSP1 oscillator sets, 1 DSP2 filter, 2 DSP3 effects, 1 clear, all reached from bank A (MIDI PC 0-63). Matches Profligacy's hardware observation | `reference/wsl/sweep_programs.sh korgpro20` then `data/traces/sweep_korgpro20/summary.json` |
| DSP1 usage over bank A: `82bce10b` 2, `78f76aa0` 21, `e504d5eb` 11, `690e8be6` 9, `3ecdfffb`/`8fdee1db`/`b4736775` 4 each, `e1a98ce3` 3, `762cd613`/`d41de4ee` 2 each, `479c76ba`/`47b6b926` 1 each. DSP3: `7894b664` 42, `2e4ace11` 22 | sweep summary |
| MIDI PC 64-127 on fresh NVRAM all give one identical patch (same programs, -23.9 dBFS), so bank B is not reached by PC alone | sweep summary |
| All 64 bank-A programs sound during the note (peaks -12 to -35 dBFS) | sweep summary |

## Phases

| Phase | Work | Done when | State |
|---|---|---|---|
| 0 | Reference oracle: build `propmin` in WSL, boot v2.0, inject a note, write WAV | Non-silent WAV from an injected note | **done** |
| 0b | Tracing patch: log PLOAD programs, CMEM init, CLOAD updates (dsp, addr, value, sample index) to `data/traces/` | Traces for boot + note + every oscillator type | **done: all 16 programs captured** |
| 1 | Envelope/LFO location: coefficient write rate during a held note | Know whether envelope/LFO run on the H8 (streamed) or inside the DSP | **answered: H8, 228.6 Hz stream** |
| 2a | Standalone DSP core: vendored, unmodified MAME TMS57002 core (BSD-3) built against `engine/mame_shim`; replayed against frame-exact oracle traces | 0 mismatches, all 16 programs | **done**: `reference/wsl/replay_programs.sh` gives 12 patches x 3 DSPs x 816,800 frames, 0 output/read mismatches; 6-9x realtime per DSP single-stepped on Ryzen 9800X3D |
| 2 | Translator: TMS57002 programs compiled to C++, bit-exact ([design](TRANSLATOR.md)); a speed optimisation over 2a | 0 mismatches vs the interpreter on all programs | **done**: 27 programs (v2.0, the clear program included), A/B 7.4M frames plain / --chaos / --budget 0 mismatches on GCC and clang builds; board 24% -> about 6% of a desktop core (`board_bench`, WSL; 1.29 s -> 0.6 s per 10 s of audio) |
| 2b | **First sound on the MPC**: A00's translated DSP1+DSP2+DSP3 chain, driven by the coefficient stream recorded from the oracle (no control layer yet), wrapped with the MPC SDK, resampled 48 to 44.1 kHz | Plays on your Live III; matches the oracle WAV in host-sim; per-voice CPU measured on device | superseded by 6 (the full engine plays on the device) |
| 3 | Static extraction: locate programs, tables and factory programs in ic22 by offset; per-version map | `extract.py` reproduces every traced program and coefficient from the ROM alone | **DSP programs + CMEM init done** (`romdata/korgpro20.json`, 16/16 match); V55 side: oscillator-model defaults (ic12 0x3C587), the 128 factory program dumps (0x3FF60, Performance Editor settings) and the PE curve table (0x77676), read at run time ([PARAMS.md](PARAMS.md)) |
| 4 | Native control layer: program bytes + MIDI to coefficient stream | Coefficient stream matches oracle for all 128 programs on test phrases | **done**: the H8 firmware translated (`tools/h8dec.py`, `tools/h8xlat.py`) under a native scheduler, the V55 side native (program bursts byte-exact vs 128 captures, Performance Editor vs the oracle, arpeggiator 34/34 scenarios); coefficient memory 0.9 s after each bank-A program change: 47,778 / 49,152 words equal; native boot from the ROMs ([H8.md](H8.md)) |
| 5 | Engine: voice = DSP1+DSP2, shared DSP3, 48 kHz internal, resampled to 44.1 kHz | Host-sim null test vs oracle | **done as the Prophecy is**: one board (monophonic), 48 kHz, resampled to the host rate; idle sleep after 1 s of silence |
| 6 | MPC plugin: SDK wrapper, programs, state, params/Q-Links | Plays on Live III; per-voice CPU measured | **plays on the Live III**: 128 presets + your SysEx, 525 program parameters, Performance Editor, arpeggiator, project restore, Stop silences; the MPC's CPU meter while playing 50-63% -> 23-26% (about 13% idle, the MPC's own); Engine Load readout and LOAD lines in the MIDI Log |
| 7 | Polyphony | N voices within CPU budget | **built**: Voices 1-8 (`engine/src/poly.h`): one complete engine per voice, each bit-exact (poly_test: 1 voice = the plain engine, 4 voices = 4 engines summed, sample for sample); cost per sounding voice ~6.5% of a desktop core (8 voices 55%), silent voices sleep; on the MPC about 10-13 points of the CPU meter per sounding voice (not yet measured there); effects run per voice (linear ones sum as one) |
| 8 | Release: build it yourself | A ROM owner gets the plugin with one command | **built**: `tools/make_plugin.py` / `Make Plugin.bat` (Python only; zig via the SDK), tested from the release zip with zipped, byte-swapped dumps: plugin binaries identical to the dev build, 4.2 min; GPL-3.0-or-later; public name "Prophetic" (uid `KpPr`); Open MPC "build-yourself" entry drafted ([RELEASE.md](RELEASE.md)) |
| 9 | Ready-made download (ROMs dropped into Content, no PC build) | The plugin binary holds nothing derived from the ROMs | **later**: needs run-time replacements for the two compiled-in parts. DSP programs: an ARM64 code generator (the interpreter costs 4.2-5.6x the compiled programs, `synth_bench` programs 0 and 21). H8 code: an interpreter or generator (the translated H8 code is about 7% of the engine's time, gprof, program 0). First step: a device test that the MPC lets a plugin run code it generates |

## Open risks

| Risk | Mitigation |
|---|---|
| v2.0 output unvalidated vs hardware | Cross-check traces against v1.7 behavior where programs overlap; record diffs |
| ~~Bank B factory programs not reached by PC 64-127~~ | resolved: all 128 factory programs from the ic12 dumps, bursts byte-exact vs the oracle's captures |
| H8 control logic is large and interacting | Parameter sweeps for single-parameter tables; targeted H8 disassembly for interactions |
| Per-voice CPU on the MPC | measured on the Live III (RK3588: 4x A76 + 4x A55); the MIDI Log's LOAD lines record the audio thread's load and core per second |
