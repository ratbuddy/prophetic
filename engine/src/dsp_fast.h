// Fast TMS57002 frame execution: ahead-of-time compiled DSP programs.
//
// tools/dsp_aot_gen (engine/tools/dsp_aot_gen.cpp) plays every program of the user's firmware
// through kp::Synth, harvests each DSP's decode cache in steady state and emits, per DSP
// program, one C++ function whose code is the interpreter's own op functions (ex_N, inlined,
// constant operands) laid out in program order with the interpreter's per-instruction
// bookkeeping between them (generated/dspaot/dsp_aot_gen.inc, firmware-derived, not committed).
//
// At run time dsp_fast_run() checks, step by step, that the live device's decode cache holds
// exactly the op chain the generated code was compiled from; any step that differs, any cache
// miss and any unknown program runs in the vendored interpreter from that exact point on.
// The result is bit-identical to tms57002_device::execute_run() by construction (and verified
// by engine/test/dsp_fast_test.cpp against the interpreter).
#pragma once

#include <array>
#include <cstdint>

class tms57002_device;

namespace kp {

struct AotProgram;

struct DspFast
{
	static constexpr int kMaxSteps = 4096;   // = decode cache hash nodes (HBS)
	static constexpr int kMaxIpc = 8192;     // = decode cache instruction slots (IBS)

	const AotProgram *prog = nullptr;
	std::uint32_t version = 0xffffffffu;     // device program version prog was selected for
	int hused = -1, iused = -1;              // decode cache size when last validated
	std::int16_t step_of_ipc[kMaxIpc];       // device chain head -> generated step (valid ones), else -1
	std::int16_t ipc_of_step[kMaxSteps];     // generated step -> device chain head, -1 if absent
	std::uint8_t valid[kMaxSteps];           // generated step matches the device's chain exactly
	int xm_first = 1;                        // external memory, for the device's st0 (set per frame,
	int xm_req_fetches = 0, xm_req_cycles = 0;   // see KP_PRE): first fetch cycle, fetches, cycles

	// Statistics (frames run through generated code, frames run by the interpreter, frames that
	// left generated code mid-way).
	std::uint64_t fast_frames = 0, interp_frames = 0, deopts = 0;
	std::uint64_t slow_posts = 0;            // steps that took the out-of-line epilogue
	int valid_steps = 0, total_steps = 0;

	void reset() { prog = nullptr; version = 0xffffffffu; hused = iused = -1; }
};

// Equivalent to `dev.execute_run()` (icount already set by the caller).
void dsp_fast_run(tms57002_device &dev, DspFast &f);

// Fault in this module's code pages ahead of time (Linux; no-op elsewhere). Call off the audio
// thread, e.g. when the synth loads.
void dsp_fast_prefault();

// Process-wide switch (A/B tests): false = always the interpreter.
void dsp_fast_enable(bool on);
bool dsp_fast_enabled();
// Tests: cap every frame's instruction budget at n (0 = off), for the interpreter and the compiled
// code alike, so frames end on the budget at arbitrary points (the board gives 1 << 16, which a
// frame never uses up).
void dsp_fast_budget_cap(int n);
// Tests: the compiled code's external memory step against the device's own (xm_step_read /
// xm_step_write) on `cases` transfers over every st0 transfer mode, from d's state (restored
// afterwards; the memory bytes it touches keep the random values). Returns the mismatches.
int dsp_fast_xm_selftest(tms57002_device &d, int cases, unsigned seed);

// The board's per-frame stream update on 24-bit words (dev.sound_stream_update() without the float
// stream): inputs in, outputs (24-bit) out. Returns false, having done nothing, when the device
// needs the stream path (its test input sine) or the switch below is off.
bool dsp_stream_update_raw(tms57002_device &dev, const std::array<std::uint32_t, 4> &in, std::array<std::uint32_t, 4> &out);
// Tests: false = the board always takes the float stream path (A/B against the raw path).
void dsp_fast_stream_raw(bool on);

// Number of generated programs compiled in (0 when built without generated/dspaot).
int dsp_fast_program_count();

} // namespace kp
