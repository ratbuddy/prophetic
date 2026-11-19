// Engine benchmark: load a program, hold a note for `seconds`, report real-time factor.
// Usage: synth_bench <data dir> [program=0] [seconds=10]
#include "synth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir> [program] [seconds]\n", argv[0]); return 2; }
	const int program = argc > 2 ? std::atoi(argv[2]) : 0;
	const double seconds = argc > 3 ? std::atof(argv[3]) : 10.0;
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	std::vector<float> l(480), r(480);
	s.program(program);
	for (int b = 0; b < 100; b++) s.render(l.data(), r.data(), 480);   // let the program load
	s.note_on(60, 100);
	const auto t0 = std::chrono::steady_clock::now();
	const int blocks = int(seconds * 100);
	for (int b = 0; b < blocks; b++) s.render(l.data(), r.data(), 480);
	const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("program %d: %.1f s of audio in %.3f s = %.2fx realtime (%.1f%% of one core)\n", program, seconds, dt, seconds / dt, 100 * dt / seconds);
	return 0;
}
