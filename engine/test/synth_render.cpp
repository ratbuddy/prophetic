// Deterministic render for build-to-build comparison: plays a fixed note/controller sequence on
// each program and prints an FNV-1a hash of the 24-bit output per program and in total. Two builds
// (e.g. -DKP_DSP_PRISTINE vs the optimized core) must print identical lines.
// Usage: synth_render <data dir> [programs=128] [interp]
#include "dsp_fast.h"
#include "synth.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir> [programs] [interp]\n", argv[0]); return 2; }
	const int programs = argc > 2 ? std::atoi(argv[2]) : 128;
	if (argc > 3 && !std::strcmp(argv[3], "interp"))
		kp::dsp_fast_enable(false);
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	std::uint64_t total = 1469598103934665603ull;
	float l[480], r[480];
	auto block = [&](std::uint64_t &h, double seconds) {
		for (int n = int(seconds * 100); n > 0; n--)
		{
			s.render(l, r, 480);
			for (int i = 0; i < 480; i++)
			{
				const std::int32_t a = std::int32_t(std::lrint(double(l[i]) * 8388608.0));
				const std::int32_t b = std::int32_t(std::lrint(double(r[i]) * 8388608.0));
				h = (h ^ std::uint32_t(a)) * 1099511628211ull;
				h = (h ^ std::uint32_t(b)) * 1099511628211ull;
			}
		}
	};
	const int n = programs < s.program_count() ? programs : s.program_count();
	for (int p = 0; p < n; p++)
	{
		std::uint64_t h = 1469598103934665603ull;
		s.program(p);
		block(h, 0.5);
		s.note_on(48, 110);
		s.note_on(60, 64);
		block(h, 0.3);
		s.midi_cc(1, 90);
		s.pitch_bend(3000);
		s.aftertouch(80);
		block(h, 0.2);
		s.note_off(48);
		s.note_off(60);
		s.midi_cc(1, 0);
		s.pitch_bend(8192);
		s.aftertouch(0);
		block(h, 0.3);
		std::printf("program %3d %016llx\n", p, (unsigned long long)h);
		total = (total ^ h) * 1099511628211ull;
	}
	std::printf("total %016llx\n", (unsigned long long)total);
	return 0;
}
