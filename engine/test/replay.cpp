// Replay a frame-exact oracle trace (reference patch, KPDEV_REPLAY_TRACE) through the standalone
// TMS57002 core and compare every frame's serial outputs and every host read.
//
// Usage: replay <host.dspN.bin> <1|2|3> [max_frames] [cmem_out.bin]
// Exit: 0 = bit-exact, 1 = mismatch/desync, 2 = usage/IO error.
// cmem_out.bin: the core's 256 coefficient words (host order u32) after the last replayed frame.
#include "emu.h"
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <iostream>

// The harness drives protected/private core members directly (one instruction at a time, the
// stream update, the status bits). Standard headers are already included above, so only the
// vendored header sees the redefinition.
#define private public
#define protected public
#include "tms57002.h"
#undef private
#undef protected

namespace {

struct Rec
{
	u32 w0, frame, steps, v[8];
	char kind() const { return char(w0 & 0xff); }
	u8 a() const { return u8((w0 >> 8) & 0xff); }
};
static_assert(sizeof(Rec) == 44, "trace record layout");

class replay_stream : public sound_stream
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

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <host.dspN.bin> <1|2|3> [max_frames]\n", argv[0]);
		return 2;
	}
	const int dspn = std::atoi(argv[2]);
	const u64 max_frames = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : std::numeric_limits<u64>::max();

	std::ifstream f(argv[1], std::ios::binary);
	if (!f)
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<Rec> recs(std::filesystem::file_size(argv[1]) / sizeof(Rec));
	f.read(reinterpret_cast<char *>(recs.data()), recs.size() * sizeof(Rec));

	machine_config cfg;
	const std::string tag = ":dsp" + std::to_string(dspn);
	tms57002_device dev(cfg, tag.c_str(), nullptr, 24576000);
	dev.m_shim_data_ram_bytes = (dspn == 2) ? 0 : 0x40000;   // korgprophecy dsp_ram_map / dsp_noram_map
	dev.device_start();
	// korgprophecy_state::machine_start DSP configuration
	dev.set_serial_frame_model(false);
	dev.set_serial_frame_clocks(512);
	dev.set_sync_polarity(1);
	if (dspn == 3)
		dev.set_serial_frame_flip_input(true);
	dev.set_serial_frame_flip_output(false);
	dev.set_smhc_trunc_bits(0);
	dev.set_serial_output_write_handoff(dspn == 1);
	if (dspn != 3)
		dev.set_stream_output_raw(false);   // korgprophecy machine config (DSP3 keeps the default)
	dev.device_reset();

	u64 steps = 0, frames = 0, host_events = 0, out_mismatch = 0, read_mismatch = 0, nonzero_out = 0;
	u64 hash = 1469598103934665603ull;
	int reported = 0;
	auto can_run = [&] { return !(dev.sti & (tms57002_device::S_IDLE | tms57002_device::IN_PLOAD)); };
	auto run_to = [&](u32 target) {
		while (steps < target && can_run())
		{
			dev.icount = 1;
			dev.execute_run();
			steps++;
		}
		return steps == target;
	};
	auto fail = [&](const char *what, const Rec &r) {
		std::fprintf(stderr, "DESYNC %s at rec frame=%u steps=%u kind=%c: core frame=%" PRIu64 " steps=%" PRIu64 " pc=%02x sti=%08x\n",
			what, r.frame, r.steps, r.kind(), u64(dev.m_sound_updates), steps, dev.pc, u32(dev.sti));
		return 1;
	};

	const auto t0 = std::chrono::steady_clock::now();
	for (const Rec &r : recs)
	{
		if (frames >= max_frames)
			break;
		const char k = r.kind();
		if (k == 'F')
		{
			if (r.frame != dev.m_sound_updates + 1)
				return fail("frame number", r);
			if (!run_to(r.steps))
				return fail("frame steps", r);
			const float in_scale = 32768.0f * ((dev.st0 & tms57002_device::ST0_SIM) ? 1.0f : 256.0f);
			replay_stream s;
			for (int i = 0; i < 4; i++)
				s.in[i] = float(sext24(r.v[i])) / in_scale;
			const bool synced = r.a() && !(dev.sti & tms57002_device::IN_PLOAD);
			dev.sound_stream_update(s);
			for (int i = 0; i < 4; i++)
			{
				const u32 got = u32(s32(double(s.out[i]) * 2147483648.0) >> 8) & 0x00ffffffU;
				const u32 want = r.v[4 + i];
				hash = (hash ^ got) * 1099511628211ull;
				if (want)
					nonzero_out++;
				if (got != want)
				{
					out_mismatch++;
					if (reported++ < 10)
						std::fprintf(stderr, "MISMATCH frame=%u ch=%d want=%06x got=%06x\n", r.frame, i, want, got);
				}
			}
			if (synced)
				steps = 0;
			frames++;
			continue;
		}

		if (r.frame != dev.m_sound_updates)
			return fail("host frame", r);
		if (!run_to(r.steps))
			return fail("host steps", r);
		host_events++;
		switch (k)
		{
		case 'L': dev.pload_w(r.a()); break;
		case 'C': dev.cload_w(r.a()); break;
		case 'D': dev.data_w(r.a()); break;
		case 'M': dev.mute_w(r.a()); break;
		case 'R':
		{
			const u8 got = dev.data_r();
			if (got != r.a())
			{
				read_mismatch++;
				if (reported++ < 10)
					std::fprintf(stderr, "READ MISMATCH frame=%u want=%02x got=%02x\n", r.frame, r.a(), got);
			}
			break;
		}
		default:
			std::fprintf(stderr, "unknown record kind %d\n", k);
			return 2;
		}
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	if (argc > 4)   // coefficient memory at the end of the replay (oracle reference for cmem checks)
	{
		std::ofstream o(argv[4], std::ios::binary);
		o.write(reinterpret_cast<const char *>(dev.cmem), sizeof dev.cmem);
	}
	std::printf("dsp%d: frames=%" PRIu64 " host_events=%" PRIu64 " nonzero_out_words=%" PRIu64
		" out_mismatch=%" PRIu64 " read_mismatch=%" PRIu64 " hash=%016" PRIx64 " time=%.2fs (%.1fx realtime)\n",
		dspn, frames, host_events, nonzero_out, out_mismatch, read_mismatch, hash, secs,
		secs > 0 ? (frames / 48000.0) / secs : 0.0);
	return (out_mismatch || read_mismatch) ? 1 : 0;
}
