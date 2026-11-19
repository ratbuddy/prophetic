#include "prophecy_board.h"
#include "dsp_fast.h"

#include "emu.h"
#include <type_traits>
#include <cstring>
#include <fstream>
#include <new>
#include <filesystem>
#include <limits>
#include <map>
#include <set>

// Drive protected/private core members (single-step, stream update, status bits) without
// modifying the vendored core. Standard headers are already included above.
#define private public
#define protected public
#include "tms57002.h"
#undef private
#undef protected

namespace kp {

namespace {

class frame_stream : public sound_stream
{
public:
	sample_t in[4] = {};
	sample_t out[4] = {};
	sample_t get(int ch, int) const override { return in[ch]; }
	void put(int ch, int, sample_t v) override { out[ch] = v; }
	int samples() const override { return 1; }
	u32 sample_rate() const override { return 48000; }
};

s32 sext24(u32 v) { return s32(v << 8) >> 8; }

} // namespace

struct ProphecyBoard::Impl
{
	machine_config cfg;
	std::unique_ptr<tms57002_device> dsp[3];
	u32 steps[3] = {};
	std::array<u32, 4> in[3] = {};
	std::array<u32, 4> out[3] = {};
	u32 feedback = 0;      // DSP2 out2 from the previous frame
	u32 optionin = 0;      // 0 on frame 0, pulled up afterwards
	u64 frames = 0;
	DspFast fast[3];       // ahead-of-time compiled program state per DSP (dsp_fast.h)

	bool runnable(int d) const
	{
		return !(dsp[d]->sti & (tms57002_device::S_IDLE | tms57002_device::IN_PLOAD));
	}
};

ProphecyBoard::ProphecyBoard() : m(std::make_unique<Impl>())
{
	for (int d = 0; d < 3; d++)
	{
		const std::string tag = ":dsp" + std::to_string(d + 1);
		auto &dev = m->dsp[d];
		// Constructed in zero-filled storage, as MAME allocates devices (global_alloc_clear): the
		// core leaves some state to that (dmem1, for one), and a natively booted board must not start
		// from whatever the heap held (it would differ between builds and runs).
		void *storage = ::operator new(sizeof(tms57002_device));
		std::memset(storage, 0, sizeof(tms57002_device));
		dev.reset(new (storage) tms57002_device(m->cfg, tag.c_str(), nullptr, 24576000));
		dev->m_shim_data_ram_bytes = (d == 1) ? 0 : 0x40000;   // dsp_ram_map / dsp_noram_map
		dev->device_start();
		// korgprophecy_state::machine_start / machine config
		dev->set_serial_frame_model(false);
		dev->set_serial_frame_clocks(512);
		dev->set_sync_polarity(1);
		if (d == 2)
			dev->set_serial_frame_flip_input(true);
		dev->set_serial_frame_flip_output(false);
		dev->set_smhc_trunc_bits(0);
		dev->set_serial_output_write_handoff(d == 0);
		if (d != 2)
			dev->set_stream_output_raw(false);
		dev->device_reset();
	}
}

ProphecyBoard::~ProphecyBoard() = default;

void ProphecyBoard::pload(int d, int state) { m->dsp[d]->pload_w(state); }
void ProphecyBoard::cload(int d, int state) { m->dsp[d]->cload_w(state); }
void ProphecyBoard::data_w(int d, std::uint8_t v) { m->dsp[d]->data_w(v); }
std::uint8_t ProphecyBoard::data_r(int d) { return m->dsp[d]->data_r(); }
void ProphecyBoard::mute(int d, int state) { m->dsp[d]->mute_w(state); }
int ProphecyBoard::update_queue(int d) const { return m->dsp[d]->update_counter_count; }
std::uint32_t ProphecyBoard::steps(int d) const { return m->steps[d]; }
tms57002_device &ProphecyBoard::device(int d) { return *m->dsp[d]; }
const DspFast &ProphecyBoard::fast(int d) const { return m->fast[d]; }
std::uint64_t ProphecyBoard::frames() const { return m->frames; }
const std::array<std::uint32_t, 4> &ProphecyBoard::inputs(int d) const { return m->in[d]; }
const std::array<std::uint32_t, 4> &ProphecyBoard::outputs(int d) const { return m->out[d]; }

void ProphecyBoard::finish(int d)
{
	// Fast path: run the rest of this frame's program in one call. The instruction count is not
	// observable afterwards, so mark it unknown; only an idle DSP can be past this point.
	if (!m->runnable(d))
		return;
	auto &dev = *m->dsp[d];
	dev.icount = 1 << 16;
	dsp_fast_run(dev, m->fast[d]);   // == dev.execute_run(), compiled programs where available
	m->steps[d] = kStepsUnknown;
}

bool ProphecyBoard::run_to(int d, std::uint32_t target)
{
	auto &dev = *m->dsp[d];
	if (m->steps[d] == kStepsUnknown)
		return !m->runnable(d);
	while (m->steps[d] < target && m->runnable(d))
	{
		dev.icount = 1;
		dev.execute_run();
		m->steps[d]++;
	}
	return m->steps[d] == target;
}

void ProphecyBoard::stream_update(int d)
{
	auto &dev = *m->dsp[d];
	std::array<u32, 4> in{};
	switch (d)
	{
	case 0: in = { m->feedback, 0, 0, 0 }; break;
	case 1: in = m->out[0]; break;
	case 2: in = { m->out[1][0], m->out[1][1], m->optionin, m->optionin }; break;
	}
	const bool synced = dev.m_stream_sync_enabled && !(dev.sti & tms57002_device::IN_PLOAD);
	if (!dsp_stream_update_raw(dev, in, m->out[d]))   // the same on 24-bit words (dsp_core.cpp)
	{
		const float in_scale = 32768.0f * ((dev.st0 & tms57002_device::ST0_SIM) ? 1.0f : 256.0f);
		frame_stream s;
		for (int i = 0; i < 4; i++)
			s.in[i] = float(sext24(in[i])) / in_scale;
		dev.sound_stream_update(s);
		for (int i = 0; i < 4; i++)
			m->out[d][i] = u32(s32(double(s.out[i]) * 2147483648.0) >> 8) & 0x00ffffffU;
	}
	m->in[d] = in;
	if (synced)
		m->steps[d] = 0;
	if (d == 2)
	{
		// Board latches update after the frame's three stream updates.
		m->feedback = m->out[1][2];
		m->optionin = 0x00ffffffU;
		m->frames++;
	}
}

namespace {
constexpr u32 kStateMagic = 0x4e53504b;   // "KPSN"
constexpr u32 kStateVersion = 1;
static_assert(std::is_trivially_copyable<tms57002_device::debug_snapshot>::value, "snapshot must be POD");

template <typename T> void put(std::vector<u8> &out, const T &v)
{
	const u8 *p = reinterpret_cast<const u8 *>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}
template <typename T> bool get(const u8 *&p, const u8 *end, T &v)
{
	if (size_t(end - p) < sizeof(T))
		return false;
	std::memcpy(&v, p, sizeof(T));
	p += sizeof(T);
	return true;
}
} // namespace

std::vector<std::uint8_t> ProphecyBoard::save_state() const
{
	std::vector<u8> out;
	put(out, kStateMagic);
	put(out, kStateVersion);
	for (int d = 0; d < 3; d++)
	{
		auto &dev = *m->dsp[d];
		put(out, dev.debug_capture_snapshot());
		for (u32 a = 0; a < 256; a++)
			put(out, dev.program->read_dword(a));
		const auto &ram = dev.data->bytes();
		put(out, u32(ram.size()));
		out.insert(out.end(), ram.begin(), ram.end());
		put(out, m->steps[d]);
		put(out, m->in[d]);
		put(out, m->out[d]);
	}
	put(out, m->feedback);
	put(out, m->optionin);
	put(out, m->frames);
	return out;
}

bool ProphecyBoard::load_state(const std::uint8_t *data, std::size_t size)
{
	const u8 *p = data, *end = data + size;
	u32 magic = 0, version = 0;
	if (!get(p, end, magic) || !get(p, end, version) || magic != kStateMagic || version != kStateVersion)
		return false;
	for (int d = 0; d < 3; d++)
	{
		auto &dev = *m->dsp[d];
		tms57002_device::debug_snapshot snap;
		if (!get(p, end, snap))
			return false;
		for (u32 a = 0; a < 256; a++)
		{
			u32 w;
			if (!get(p, end, w))
				return false;
			dev.program->write_dword(a, w);
		}
		u32 ram_size = 0;
		auto &ram = dev.data->bytes();
		if (!get(p, end, ram_size) || ram_size != ram.size() || size_t(end - p) < ram_size)
			return false;
		std::memcpy(ram.data(), p, ram_size);
		p += ram_size;
		dev.debug_restore_snapshot(snap);
		// The decode cache is keyed on the (restored) program words. Flush it through the public
		// host interface: a PLOAD pulse flushes the cache and resets pc/ca/hidx and the load
		// sub-state, which are then restored. (Calling the private cache_flush() directly does
		// not link under MSVC, which mangles member access into symbol names.)
		{
			const auto pc = dev.pc;
			const auto ca = dev.ca;
			const auto hidx = dev.hidx;
			const auto sti = dev.sti;
			dev.pload_w(0);
			dev.pload_w(1);
			dev.pc = pc;
			dev.ca = ca;
			dev.hidx = hidx;
			dev.sti = sti;
		}
		if (!get(p, end, m->steps[d]) || !get(p, end, m->in[d]) || !get(p, end, m->out[d]))
			return false;
	}
	return get(p, end, m->feedback) && get(p, end, m->optionin) && get(p, end, m->frames);
}

void ProphecyBoard::frame(std::int32_t &left, std::int32_t &right)
{
	for (int d = 0; d < 3; d++)
	{
		finish(d);
		stream_update(d);
	}
	left = sext24(m->out[2][0]);
	right = sext24(m->out[2][1]);
}

} // namespace kp
