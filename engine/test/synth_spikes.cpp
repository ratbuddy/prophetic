// Worst-case block times: render 139-frame blocks (one 128-frame block at 44.1 kHz after
// resampling) while holding a note, change program mid-way, and report the slowest blocks.
// Usage: synth_spikes <data dir> [from_program=0] [to_program=1]
#include "synth.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir> [from] [to]\n", argv[0]); return 2; }
	const int from = argc > 2 ? std::atoi(argv[2]) : 0, to = argc > 3 ? std::atoi(argv[3]) : 1;
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	constexpr int kBlock = 139;
	const double budget_us = 128.0 / 44100.0 * 1e6;
	float l[kBlock], r[kBlock];
	s.program(from);
	for (int b = 0; b < 600; b++) s.render(l, r, kBlock);
	s.note_on(60, 100);
	struct T { double us; int block; };
	std::vector<T> steady, change;
	for (int b = 0; b < 3000; b++)
	{
		if (b == 1500)
			s.program(to);
		const auto t0 = std::chrono::steady_clock::now();
		s.render(l, r, kBlock);
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		(b < 1500 ? steady : change).push_back({ us, b });
	}
	auto report = [&](const char *name, std::vector<T> v) {
		double sum = 0;
		for (auto &t : v) sum += t.us;
		std::sort(v.begin(), v.end(), [](const T &a, const T &b) { return a.us > b.us; });
		std::printf("%s: mean %.0f us (%.0f%%), worst", name, sum / v.size(), 100 * sum / v.size() / budget_us);
		for (int i = 0; i < 5; i++) std::printf(" %.0f us (%.0f%%, block %d)", v[i].us, 100 * v[i].us / budget_us, v[i].block);
		std::printf("\n");
	};
	std::printf("budget per 128-frame block at 44.1 kHz: %.0f us\n", budget_us);
	report("steady (note held)", steady);
	report("program change at block 1500", change);
	return 0;
}
