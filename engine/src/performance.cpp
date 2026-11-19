#include "performance.h"

#include <cstring>

namespace kp {

void PerformancePlayer::frame(ProphecyBoard &board, std::int32_t &left, std::int32_t &right)
{
	for (int d = 0; d < 3; d++)
	{
		while (m_active && m_cursor < m_n && m_ev[m_cursor].frame == m_frame && (m_ev[m_cursor].dsp_kind >> 4) == d)
		{
			const PerfEvent &e = m_ev[m_cursor++];
			board.run_to(d, e.steps);
			switch (e.dsp_kind & 0x0f)
			{
			case kPload: board.pload(d, e.value); break;
			case kCload: board.cload(d, e.value); break;
			case kDataW: board.data_w(d, e.value); break;
			case kMute: board.mute(d, e.value); break;
			case kDataR: board.data_r(d); break;
			}
		}
		board.finish(d);
		board.stream_update(d);
	}
	if (m_active && ++m_frame && m_cursor >= m_n)
		m_active = false;
	left = std::int32_t(board.outputs(2)[0] << 8) >> 8;
	right = std::int32_t(board.outputs(2)[1] << 8) >> 8;
}

namespace {
constexpr std::uint32_t kEvMagic = 0x5645504b;   // "KPEV"
}

bool parse_events(const std::uint8_t *data, std::size_t size, std::vector<PerfEvent> &out)
{
	std::uint32_t hdr[3];
	if (size < sizeof(hdr))
		return false;
	std::memcpy(hdr, data, sizeof(hdr));
	if (hdr[0] != kEvMagic || hdr[1] != 1 || size != sizeof(hdr) + std::size_t(hdr[2]) * sizeof(PerfEvent))
		return false;
	out.resize(hdr[2]);
	std::memcpy(out.data(), data + sizeof(hdr), out.size() * sizeof(PerfEvent));
	return true;
}

std::vector<std::uint8_t> serialize_events(const std::vector<PerfEvent> &events)
{
	const std::uint32_t hdr[3] = { kEvMagic, 1, std::uint32_t(events.size()) };
	std::vector<std::uint8_t> out(sizeof(hdr) + events.size() * sizeof(PerfEvent));
	std::memcpy(out.data(), hdr, sizeof(hdr));
	std::memcpy(out.data() + sizeof(hdr), events.data(), events.size() * sizeof(PerfEvent));
	return out;
}

} // namespace kp
