// Prophecy DSP board: three TMS57002 cores wired as measured from oracle traces
// (tools/wiring_check.py on korgpro20, 100% match on every live lane):
//   DSP1 in0   = DSP2 out2 (SO1-L), one frame late (feedback latch); DSP1 in1..3 = 0 (SDN)
//   DSP2 in0..3 = DSP1 out0..3, same frame
//   DSP3 in0,1 = DSP2 out0,1, same frame; DSP3 in2,3 = OPTIONIN (0 on frame 0, then 0xffffff)
//   DAC L/R    = DSP3 out0/out1 (24-bit)
// Stream-update order per 48 kHz frame: DSP1, DSP2, DSP3 (MAME pull order).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class tms57002_device;

namespace kp {

struct DspFast;

class ProphecyBoard
{
public:
	static constexpr int kSampleRate = 48000;

	ProphecyBoard();
	~ProphecyBoard();
	ProphecyBoard(const ProphecyBoard &) = delete;
	ProphecyBoard &operator=(const ProphecyBoard &) = delete;

	// Host interface of DSP d (0..2), as driven by the H8.
	void pload(int d, int state);
	void cload(int d, int state);
	void data_w(int d, std::uint8_t v);
	std::uint8_t data_r(int d);
	void mute(int d, int state);
	// Number of queued (not yet applied) coefficient updates in DSP d (hardware limit 16).
	int update_queue(int d) const;

	// Execute DSP d (single-stepping) until `steps` instructions have run since its last sync, or
	// it idles. Returns true if exactly `steps` was reached (host events are positioned this way).
	bool run_to(int d, std::uint32_t steps);
	// Run the rest of DSP d's frame program in one call (fast path; instruction count unknown after).
	void finish(int d);
	std::uint32_t steps(int d) const;
	static constexpr std::uint32_t kStepsUnknown = 0xffffffffu;

	// Per-DSP stream update for the next frame: compute inputs from the wiring, latch outputs,
	// sync. Call for d = 0, 1, 2 in order once per frame (frame() does all three).
	void stream_update(int d);
	// Finish the frame's programs and advance one frame. Returns DAC L/R as signed 24-bit.
	void frame(std::int32_t &left, std::int32_t &right);

	const std::array<std::uint32_t, 4> &inputs(int d) const;   // last frame inputs per DSP
	const std::array<std::uint32_t, 4> &outputs(int d) const;  // last frame outputs per DSP
	std::uint64_t frames() const;

	// Whole-board state (3 x core snapshot, program memory, external RAM, board latches).
	// Firmware-derived once programs are loaded: keep under data/ or generated/, never commit.
	std::vector<std::uint8_t> save_state() const;
	bool load_state(const std::uint8_t *data, std::size_t size);   // no allocation; audio-thread safe

	// Diagnostics / tools: the core of DSP d and its compiled-program state (dsp_fast.h).
	tms57002_device &device(int d);
	const DspFast &fast(int d) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};

} // namespace kp
