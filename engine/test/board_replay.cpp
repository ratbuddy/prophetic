// End-to-end board check: drive the wired 3-DSP board with all three DSPs' recorded host events
// (frame-exact positions) and compare every DSP's inputs (wiring) and outputs per frame against
// the oracle. Then time free-running frames (fast path) for a CPU estimate.
//
// Usage: board_replay <replay dir with host.dsp{1,2,3}.bin> [free_run_seconds=10]
#include "prophecy_board.h"

#include <chrono>
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
static_assert(sizeof(Rec) == 44, "trace record layout");

std::vector<Rec> load(const std::string &path)
{
	std::vector<Rec> r(std::filesystem::file_size(path) / sizeof(Rec));
	std::ifstream(path, std::ios::binary).read(reinterpret_cast<char *>(r.data()), r.size() * sizeof(Rec));
	return r;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <replay dir> [free_run_seconds]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1];
	const double free_secs = argc > 2 ? std::atof(argv[2]) : 10.0;
	std::vector<Rec> recs[3];
	for (int d = 0; d < 3; d++)
		recs[d] = load(dir + "/host.dsp" + std::to_string(d + 1) + ".bin");

	kp::ProphecyBoard board;
	size_t idx[3] = {};
	std::uint64_t in_mismatch = 0, out_mismatch = 0, read_mismatch = 0, frames = 0;
	int reported = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		bool done = false;
		for (int d = 0; d < 3 && !done; d++)
		{
			for (;;)
			{
				if (idx[d] >= recs[d].size())
				{
					done = true;
					break;
				}
				const Rec &r = recs[d][idx[d]++];
				if (r.kind() == 'F')
				{
					if (r.frame != board.frames() + 1 || !board.run_to(d, r.steps))
					{
						std::fprintf(stderr, "DESYNC dsp%d F rec frame=%u steps=%u board frame=%" PRIu64 " steps=%u\n",
							d + 1, r.frame, r.steps, board.frames(), board.steps(d));
						return 1;
					}
					board.stream_update(d);
					for (int i = 0; i < 4; i++)
					{
						if (board.inputs(d)[i] != r.v[i])
						{
							in_mismatch++;
							if (reported++ < 10)
								std::fprintf(stderr, "IN  MISMATCH dsp%d frame=%u lane=%d want=%06x got=%06x\n", d + 1, r.frame, i, r.v[i], board.inputs(d)[i]);
						}
						if (board.outputs(d)[i] != r.v[4 + i])
						{
							out_mismatch++;
							if (reported++ < 10)
								std::fprintf(stderr, "OUT MISMATCH dsp%d frame=%u lane=%d want=%06x got=%06x\n", d + 1, r.frame, i, r.v[4 + i], board.outputs(d)[i]);
						}
					}
					break;
				}
				if (r.frame != board.frames() || !board.run_to(d, r.steps))
				{
					std::fprintf(stderr, "DESYNC dsp%d %c rec frame=%u steps=%u board frame=%" PRIu64 " steps=%u\n",
						d + 1, r.kind(), r.frame, r.steps, board.frames(), board.steps(d));
					return 1;
				}
				switch (r.kind())
				{
				case 'L': board.pload(d, r.a()); break;
				case 'C': board.cload(d, r.a()); break;
				case 'D': board.data_w(d, r.a()); break;
				case 'M': board.mute(d, r.a()); break;
				case 'R':
					if (board.data_r(d) != r.a())
						read_mismatch++;
					break;
				}
			}
		}
		if (done)
			break;
		frames++;
	}
	const double replay_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("board replay: frames=%" PRIu64 " in_mismatch=%" PRIu64 " out_mismatch=%" PRIu64 " read_mismatch=%" PRIu64
		" time=%.2fs (%.2fx realtime, single-stepped)\n", frames, in_mismatch, out_mismatch, read_mismatch, replay_s, frames / 48000.0 / replay_s);

	// Free-run the board (no host events, fast path) to estimate steady-state CPU.
	const std::uint64_t n = std::uint64_t(free_secs * 48000);
	std::int64_t energy = 0;
	const auto t1 = std::chrono::steady_clock::now();
	for (std::uint64_t i = 0; i < n; i++)
	{
		std::int32_t l, r;
		board.frame(l, r);
		energy += std::int64_t(l < 0 ? -l : l);
	}
	const double free_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	std::printf("free run: %.1fs audio in %.3fs = %.1fx realtime (%.1f%% of this core), sum|L|=%" PRId64 "\n",
		free_secs, free_s, free_secs / free_s, 100.0 * free_s / free_secs, energy);
	return (in_mismatch || out_mismatch || read_mismatch) ? 1 : 0;
}
