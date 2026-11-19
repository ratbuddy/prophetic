# TMS57002 programs compiled to C++ (Phase 2)

The DSP programs the firmware loads are compiled ahead of time into C++ frame functions that are
bit-identical to the vendored MAME interpreter (`tms57002_device::execute_run`). Nothing is
emulated differently: the compiled code runs the interpreter's own op semantics in the
interpreter's order, with the decode, dispatch and memory traffic taken out.

## Pieces

| File | Role | Tracked |
|---|---|---|
| `engine/src/dsp_core.cpp` | The vendored core as ONE translation unit (includes `tms57002.cpp`, `tms57kdec.cpp`, `tmsops.cpp`), every member public; inline `get_cmem` fast path for the interpreter; the compiled-program runtime (`dsp_fast_run`, validation, hand-off) | yes |
| `engine/src/dsp_fast.h` | Per-DSP runtime state (`DspFast`), `dsp_fast_run()` = `execute_run()` | yes |
| `tools/tms_regops.py` | From the vendored op table only: one register-mode macro per op (`KPR_N`) | yes |
| `engine/tools/dsp_aot_gen.cpp` | Plays all programs through `kp::Synth`, harvests each DSP's decode cache, writes the frame functions | yes |
| `<gen>/cpu/tms57002/tms57002_regops.hxx` | `tms_regops.py` output (no firmware data) | no (build output) |
| `generated/dspaot/dsp_aot_gen.inc` | Frame functions for your firmware's 24 DSP programs | **no** (firmware-derived) |

## Execution model reproduced

| Interpreter (`execute_run`) | Compiled frame function |
|---|---|
| `decode_get_pc()` hash lookup at (pc, st1 & ST1_CACHE), chained decode runs | Harvested at build time: one step per hash node, its op chain and chained successor become constants |
| Per instruction: external memory step, `macc_read = macc_write; macc_write = macc` | `KP_PRE`, on locals |
| Op chain: `switch` over 1736 op numbers, `ex_N(icd)` | `KPR_N(param)`: the `ex_N` body with `i->param` constant, inlined |
| Epilogue: pending pre-transfer, icount, rptc repeat, branch, pc++/wrap, rptc_next | `KP_POST` inline when nothing is pending, else the interpreter's epilogue (`slow_post`) |
| Lookup after a branch or at frame start | `lookup:` -> dispatch over the program's entry points only (frame start, branch targets, the pc after a run end, rptk repeats) |

## Register-mode op bodies (`tms_regops.py`)

| Rule | Detail |
|---|---|
| Promoted to locals | `macc macc_read macc_write aacc ca id st1 sti` (+ `ba0` read-only, the instruction budget) |
| Other members | `dv.<member>`; enum constants `D::<NAME>` |
| Helper calls | Per top-level statement: promoted state the helper reads or writes is written to the device first, state it writes is reloaded after. Read/write sets come from the helper sources, recursively; a helper whose body is not found is assumed to touch everything |
| `get_cmem` | `KP_CMEM`: with no coefficient update queued it is exactly `sti &= ~S_UPDATE; view(st1, cmem[a])` on the locals; otherwise the real `get_cmem` |
| Conflicting statements | Where a helper writes state the statement itself uses: everything is written back, the statement runs on device memory, everything is reloaded (176 of 4784 statements, rare ops) |
| Debug forwards | `mpy_d_operand` / `lmhd_d_operand` return their operand; `dsp_fast_run` uses the interpreter whenever either debug knob is set |

## Run-time safety: compiled code runs only where it is exact

| Check | Action |
|---|---|
| Program identity (`jit_program_hash` on every program load) | No compiled program -> interpreter |
| Every step vs the live decode cache (ops, operands, terminator, chained successor), re-checked whenever the cache grows | Mismatching steps (and every step that chains into one) are not entered |
| Lookup misses, non-entry steps, invalid steps | Hand-off: the interpreter continues from that exact chain head (`jit_run_rest`) |
| Serial cycle model, runtime dynarec, debug forwards | Interpreter for the whole frame |

## Gates

| Gate | Command | Result (korgpro20, 2026-11-16) |
|---|---|---|
| A/B, every sample and DSP output word, interpreter vs compiled | `dsp_fast_test <data> 128` | PASS, 7,387,200 frames, 0 mismatches (clang and MSVC builds) |
| Forced hand-offs at random steps | `dsp_fast_test <data> 24 --chaos` | PASS, 3.0M hand-offs, 0 mismatches |
| Pristine vendored core vs optimized build, interpreter and compiled | `synth_render <data> 128 [interp]` in `-DKP_DSP_PRISTINE` and normal builds | identical hashes, 128 programs |

## Speed (Ryzen 9800X3D, one core, clang -O2 as in the MPC build)

| Measure | Interpreter (pristine) | Compiled |
|---|---|---|
| Board only (`board_bench`) | 24.1% | 12.2% |
| Full synth, held note (`synth_bench`, programs 0/40/100) | 24.8 / 22.3 / 21.2% | 12.7 / 13.6 / 12.0% |
| MPC plugin in host_sim, 128-frame blocks, average / worst | 33-36% / 72-96% | 12.6% / 33% |

What remains is mostly the per-op work itself and the unavoidable stores of every result to DSP
memory; on aarch64 (31 registers) the locals spill far less than on x86-64.

## Build

| Build | Steps |
|---|---|
| Linux/WSL | `make -C engine dspaot` (generator + harvest, about 75 s), then `make -C engine fast` (gates and benches) |
| Windows app | `apps\win\build.ps1` generates both files when the `.inc` is missing (`-Regen` to force) |
| MPC plugin | `py tools/stage_mpc.py` stages the `.inc` and generates the regops header; without the `.inc` the plugin uses the interpreter |
