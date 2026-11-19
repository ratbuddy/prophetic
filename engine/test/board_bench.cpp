// DSP board benchmark: restore the engine snapshot (state.bin) and run board frames only (no H8
// control layer), compiled programs vs interpreter.
// Usage: board_bench <data dir> [seconds=10] [interp|fast]
#include "dsp_fast.h"
#include "prophecy_board.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir> [seconds]\n", argv[0]); return 2; }
	const double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;
	std::ifstream f(std::string(argv[1]) + "/state.bin", std::ios::binary);
	const std::vector<std::uint8_t> state((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	const std::string only = argc > 3 ? argv[3] : "";
	for (int fast = 0; fast < 2; fast++)
	{
		if ((only == "interp" && fast) || (only == "fast" && !fast))
			continue;
		kp::dsp_fast_enable(fast != 0);
		kp::ProphecyBoard b;
		if (!b.load_state(state.data(), state.size())) { std::fprintf(stderr, "bad state.bin\n"); return 2; }
		std::int32_t l, r;
		long long sum = 0;
		for (int i = 0; i < 4800; i++) b.frame(l, r);
		const int frames = int(seconds * kp::ProphecyBoard::kSampleRate);
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < frames; i++) { b.frame(l, r); sum += l ^ r; }
		const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		for (int d = 0; d < 3; d++)
			std::printf("  dsp%d: %llu compiled frames, %llu interpreted, %llu hand-offs, %llu slow epilogues, %d/%d valid steps\n", d + 1,
			            (unsigned long long)b.fast(d).fast_frames, (unsigned long long)b.fast(d).interp_frames, (unsigned long long)b.fast(d).deopts,
			            (unsigned long long)b.fast(d).slow_posts, b.fast(d).valid_steps, b.fast(d).total_steps);
		std::printf("%s: %.1f s of audio in %.3f s = %.1f%% of one core (checksum %lld; compiled frames %llu/%llu/%llu)\n",
		            fast ? "compiled   " : "interpreter", seconds, dt, 100 * dt / seconds, sum,
		            (unsigned long long)b.fast(0).fast_frames, (unsigned long long)b.fast(1).fast_frames, (unsigned long long)b.fast(2).fast_frames);
	}
	return 0;
}
