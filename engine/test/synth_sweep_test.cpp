// Which controllers change a held note? For each program and controller, render a held note with
// and without a controller sweep (0 -> 127 -> 0 over 1.5 s, starting 0.5 s into the note) and report
// how different the sweep render is from the baseline during the sweep window.
// Usage: synth_sweep_test <data dir> <program> [<program> ...]
#include "synth.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Ctl { const char *name; int kind; int num; };   // kind 0 = CC, 1 = bend, 2 = aftertouch

std::vector<float> render(const std::string &dir, int program, const Ctl *c)
{
	kp::Synth s;
	std::string err;
	if (!s.load(dir, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); std::exit(2); }
	std::vector<float> out, l(480), r(480);
	auto run = [&](int blocks) { for (int b = 0; b < blocks; b++) { s.render(l.data(), r.data(), 480); out.insert(out.end(), l.begin(), l.end()); } };
	s.program(program);
	run(60);                 // 0.6 s: program load
	s.note_on(60, 100);
	run(50);                 // 0.5 s into the note
	for (int b = 0; b < 150; b++)   // 1.5 s sweep window
	{
		if (c)
		{
			const int v = b < 75 ? b * 127 / 75 : (150 - b) * 127 / 75;
			if (c->kind == 0) s.midi_cc(c->num, v);
			else if (c->kind == 1) s.pitch_bend(8192 + (v * 8191) / 127);
			else s.aftertouch(v);
		}
		run(1);
	}
	s.note_off(60);
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) { std::fprintf(stderr, "usage: %s <data dir> <program>...\n", argv[0]); return 2; }
	const Ctl ctls[] = { { "CC1 mod", 0, 1 }, { "CC2 breath", 0, 2 }, { "CC4 foot", 0, 4 }, { "CC11 expr", 0, 11 },
		{ "CC16", 0, 16 }, { "CC17", 0, 17 }, { "CC18", 0, 18 }, { "CC19", 0, 19 }, { "bend", 1, 0 }, { "aftertouch", 2, 0 } };
	for (int a = 2; a < argc; a++)
	{
		const int p = std::atoi(argv[a]);
		const auto base = render(argv[1], p, nullptr);
		const size_t w0 = 110 * 480, w1 = 260 * 480;
		double eb = 0;
		for (size_t i = w0; i < w1; i++) eb += double(base[i]) * base[i];
		std::printf("program %d:", p);
		for (const Ctl &c : ctls)
		{
			const auto x = render(argv[1], p, &c);
			double ed = 0;
			for (size_t i = w0; i < w1; i++) ed += double(x[i] - base[i]) * (x[i] - base[i]);
			std::printf("  %s %+.0f dB", c.name, ed > 0 ? 10 * std::log10(ed / (eb + 1e-30)) : -999.0);
		}
		std::printf("\n");
	}
	return 0;
}
