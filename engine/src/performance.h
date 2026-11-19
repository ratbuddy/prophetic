// A recorded H8 -> DSP control stream ("performance") played into a ProphecyBoard.
// Events are positioned exactly (frame, instruction count since sync), as captured from the
// oracle. Firmware-derived (program-load bytes): generated locally, never committed.
#pragma once

#include "prophecy_board.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kp {

struct PerfEvent
{
	std::uint32_t frame;   // frames since the performance start
	std::uint16_t steps;   // instructions since that frame's sync
	std::uint8_t dsp_kind; // dsp (0..2) << 4 | kind (0=PLOAD 1=CLOAD 2=DATA_W 3=MUTE 4=DATA_R)
	std::uint8_t value;
};
static_assert(sizeof(PerfEvent) == 8, "event layout");

enum PerfKind : std::uint8_t { kPload = 0, kCload = 1, kDataW = 2, kMute = 3, kDataR = 4 };

class PerformancePlayer
{
public:
	// Events must be sorted by (frame, dsp), original order within each.
	void set_events(const PerfEvent *events, std::size_t count) { m_ev = events; m_n = count; stop(); }
	void start() { m_cursor = 0; m_frame = 0; m_active = m_n != 0; }
	void stop() { m_active = false; }
	bool active() const { return m_active; }
	std::uint32_t position() const { return m_frame; }

	// Advance the board one 48 kHz frame, applying this frame's events. Returns DAC L/R (24-bit).
	void frame(ProphecyBoard &board, std::int32_t &left, std::int32_t &right);

private:
	const PerfEvent *m_ev = nullptr;
	std::size_t m_n = 0, m_cursor = 0;
	std::uint32_t m_frame = 0;
	bool m_active = false;
};

// File format: "KPEV" u32 version=1, u32 count, PerfEvent[count].
bool parse_events(const std::uint8_t *data, std::size_t size, std::vector<PerfEvent> &out);
std::vector<std::uint8_t> serialize_events(const std::vector<PerfEvent> &events);

} // namespace kp
