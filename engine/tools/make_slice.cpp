// Build a playable "slice" from a frame-exact oracle capture: the board state at the note start,
// plus the recorded H8 control stream from there on. Verifies that restoring the state and playing
// the stream with the plugin's scheduler (PerformancePlayer, fast path) reproduces the oracle
// bit-exactly on every DSP, and writes a 48 kHz WAV of the DAC output for listening.
//
// Usage: make_slice <replay dir> <out dir> [start_frame=576000]
// Out: state.bin, events.bin, slice.wav  (firmware-derived: out dir must be under data/ or generated/)
#include "performance.h"
#include "prophecy_board.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Rec
{
	std::uint32_t w0, frame, steps, v[8];
	char kind() const { return char(w0 & 0xff); }
	std::uint8_t a() const { return std::uint8_t((w0 >> 8) & 0xff); }
};

std::vector<Rec> load(const std::string &path)
{
	std::vector<Rec> r(std::filesystem::file_size(path) / sizeof(Rec));
	std::ifstream(path, std::ios::binary).read(reinterpret_cast<char *>(r.data()), r.size() * sizeof(Rec));
	return r;
}

int kind_code(char k)
{
	switch (k)
	{
	case 'L': return kp::kPload;
	case 'C': return kp::kCload;
	case 'D': return kp::kDataW;
	case 'M': return kp::kMute;
	case 'R': return kp::kDataR;
	}
	return -1;
}

void write_wav(const std::string &path, const std::vector<std::int16_t> &lr)
{
	std::ofstream f(path, std::ios::binary);
	auto u32le = [&](std::uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16le = [&](std::uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
	const std::uint32_t bytes = std::uint32_t(lr.size() * 2);
	f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
	u32le(48000); u32le(48000 * 4); u16le(4); u16le(16); f.write("data", 4); u32le(bytes);
	f.write(reinterpret_cast<const char *>(lr.data()), bytes);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <replay dir> <out dir> [start_frame]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1], out = argv[2];
	const std::uint64_t split = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 576000;
	std::filesystem::create_directories(out);
	std::vector<Rec> recs[3];
	for (int d = 0; d < 3; d++)
		recs[d] = load(dir + "/host.dsp" + std::to_string(d + 1) + ".bin");

	// 1. Replay boot exactly up to the split frame.
	kp::ProphecyBoard a;
	size_t idx[3] = {};
	while (a.frames() < split)
	{
		for (int d = 0; d < 3; d++)
		{
			for (;;)
			{
				if (idx[d] >= recs[d].size())
				{
					std::fprintf(stderr, "trace ends before split frame\n");
					return 1;
				}
				const Rec &r = recs[d][idx[d]++];
				if (!a.run_to(d, r.steps))
				{
					std::fprintf(stderr, "DESYNC dsp%d during boot\n", d + 1);
					return 1;
				}
				if (r.kind() == 'F')
				{
					a.stream_update(d);
					break;
				}
				switch (r.kind())
				{
				case 'L': a.pload(d, r.a()); break;
				case 'C': a.cload(d, r.a()); break;
				case 'D': a.data_w(d, r.a()); break;
				case 'M': a.mute(d, r.a()); break;
				case 'R': a.data_r(d); break;
				}
			}
		}
	}
	const std::vector<std::uint8_t> state = a.save_state();
	std::ofstream(out + "/state.bin", std::ios::binary).write(reinterpret_cast<const char *>(state.data()), state.size());

	// 2. Segment events and expected outputs from the split on.
	std::vector<kp::PerfEvent> events;
	std::vector<std::vector<std::array<std::uint32_t, 4>>> expect(3);
	for (int d = 0; d < 3; d++)
	{
		for (size_t i = idx[d]; i < recs[d].size(); i++)
		{
			const Rec &r = recs[d][i];
			if (r.kind() == 'F')
			{
				expect[d].push_back({ r.v[4], r.v[5], r.v[6], r.v[7] });
				continue;
			}
			const int k = kind_code(r.kind());
			if (k < 0 || r.steps > 0xffff)
			{
				std::fprintf(stderr, "unencodable record\n");
				return 1;
			}
			events.push_back({ std::uint32_t(r.frame - split), std::uint16_t(r.steps), std::uint8_t((d << 4) | k), r.a() });
		}
	}
	std::stable_sort(events.begin(), events.end(), [](const kp::PerfEvent &x, const kp::PerfEvent &y) {
		return x.frame != y.frame ? x.frame < y.frame : (x.dsp_kind >> 4) < (y.dsp_kind >> 4);
	});
	const std::vector<std::uint8_t> ev_bytes = kp::serialize_events(events);
	std::ofstream(out + "/events.bin", std::ios::binary).write(reinterpret_cast<const char *>(ev_bytes.data()), ev_bytes.size());

	// 3. Verify: fresh board + restored state + plugin scheduler == oracle.
	kp::ProphecyBoard b;
	if (!b.load_state(state.data(), state.size()))
	{
		std::fprintf(stderr, "load_state failed\n");
		return 1;
	}
	std::vector<kp::PerfEvent> parsed;
	kp::parse_events(ev_bytes.data(), ev_bytes.size(), parsed);
	kp::PerformancePlayer player;
	player.set_events(parsed.data(), parsed.size());
	player.start();
	const size_t n = std::min({ expect[0].size(), expect[1].size(), expect[2].size() });
	std::uint64_t mismatch = 0;
	std::vector<std::int16_t> wav;
	wav.reserve(n * 2);
	for (size_t f = 0; f < n; f++)
	{
		std::int32_t l, r;
		player.frame(b, l, r);
		for (int d = 0; d < 3; d++)
			if (b.outputs(d) != expect[d][f] && mismatch++ < 10)
				std::fprintf(stderr, "MISMATCH frame+%zu dsp%d\n", f, d + 1);
		wav.push_back(std::int16_t(l >> 8));
		wav.push_back(std::int16_t(r >> 8));
	}
	write_wav(out + "/slice.wav", wav);
	std::printf("slice: state=%zu bytes, events=%zu (%zu bytes), frames=%zu (%.2fs), verify mismatches=%" PRIu64 "\n",
		state.size(), events.size(), ev_bytes.size(), n, n / 48000.0, mismatch);
	return mismatch ? 1 : 0;
}
