// Polyphony (engine/src/poly.h): one voice is the Prophecy exactly (= a plain Synth, sample for
// sample); N voices are N Prophecies, each playing its own note (= the sum of separate Synths fed
// the same program and one note each, sample for sample); a note beyond N takes the voice held
// longest. Also the render cost per voice.
// Usage: poly_test <data dir>
#include "poly.h"
#include "synth.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int fails = 0;
void check(bool ok, const std::string &what)
{
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok)
		fails++;
}

constexpr int kBlock = 480;

// A timeline of events at block boundaries, played on anything with note_on / note_off / render.
struct Ev { int block; bool on; int note, vel; };

template <class S> std::vector<float> play(S &s, const std::vector<Ev> &evs, int blocks)
{
	std::vector<float> out;
	out.reserve(size_t(blocks) * kBlock * 2);
	float l[kBlock], r[kBlock];
	size_t e = 0;
	for (int b = 0; b < blocks; b++)
	{
		for (; e < evs.size() && evs[e].block == b; e++)
			evs[e].on ? s.note_on(evs[e].note, evs[e].vel) : s.note_off(evs[e].note);
		s.render(l, r, kBlock);
		for (int i = 0; i < kBlock; i++) { out.push_back(l[i]); out.push_back(r[i]); }
	}
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir>\n", argv[0]); return 2; }
	std::string err;
	auto t0 = std::chrono::steady_clock::now();
	auto poly = std::make_unique<kp::PolySynth>();
	if (!poly->load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	poly->load_all();
	const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("     %d voices loaded in %.2f s\n", kp::PolySynth::kMaxVoices, load_s);

	for (int prog : { 21, 0 })
	{
		// 1. one voice = the Prophecy (a legato line: a mono synth's own behaviour)
		{
			auto fresh = std::make_unique<kp::PolySynth>();   // both sides from the snapshot
			fresh->load(argv[1], err);
			fresh->load_all();
			kp::PolySynth &p = *fresh;
			p.set_voices(1);
			p.program(prog);
			kp::Synth s;
			s.load(argv[1], err);
			s.program(prog);
			const std::vector<Ev> line = { { 40, true, 60, 100 }, { 70, true, 64, 90 }, { 90, false, 60, 0 }, { 120, false, 64, 0 } };
			const auto a = play(p, line, 200), b = play(s, line, 200);
			check(a == b, "program " + std::to_string(prog) + ", 1 voice: the output equals the Prophecy's (a plain engine), sample for sample");
		}
		// 2. four voices, a chord, then a fifth note: = separate engines, one note each
		{
			auto pp = std::make_unique<kp::PolySynth>();
			pp->load(argv[1], err);
			pp->load_all();
			pp->set_voices(4);
			pp->program(prog);
			const std::vector<Ev> chord = { { 40, true, 48, 100 }, { 45, true, 55, 90 }, { 50, true, 60, 80 }, { 55, true, 64, 70 },
			                                { 120, true, 67, 100 },   // fifth note: takes voice 0 (held longest)
			                                { 160, false, 55, 0 }, { 160, false, 60, 0 }, { 160, false, 64, 0 }, { 160, false, 67, 0 } };
			const auto mixed = play(*pp, chord, 260);
			// the reference: voice k plays only what the allocator gave it
			const std::vector<std::vector<Ev>> per = {
				{ { 40, true, 48, 100 }, { 120, false, 48, 0 }, { 120, true, 67, 100 }, { 160, false, 67, 0 } },
				{ { 45, true, 55, 90 }, { 160, false, 55, 0 } },
				{ { 50, true, 60, 80 }, { 160, false, 60, 0 } },
				{ { 55, true, 64, 70 }, { 160, false, 64, 0 } } };
			std::vector<std::vector<float>> outs;
			for (int k = 0; k < 4; k++)
			{
				kp::Synth s;
				s.load(argv[1], err);
				s.program(prog);
				outs.push_back(play(s, per[size_t(k)], 260));
			}
			std::vector<float> sum(outs[0]);
			for (int k = 1; k < 4; k++)
				for (size_t i = 0; i < sum.size(); i++)
					sum[i] += outs[size_t(k)][i];
			check(mixed == sum, "program " + std::to_string(prog) + ", 4 voices: a chord + a fifth note = 4 engines with one note each (the fifth on the voice held longest), summed");
			double peak = 0, sq = 0, sq1 = 0;
			for (float v : mixed) { peak = v > peak ? v : (-v > peak ? -v : peak); sq += double(v) * v; }
			for (float v : outs[1]) sq1 += double(v) * v;
			std::printf("     program %d: chord peak %.3f, rms %.4f (one of its voices alone: rms %.4f)\n", prog, peak,
			            std::sqrt(sq / double(mixed.size())), std::sqrt(sq1 / double(outs[1].size())));
		}
	}
	// 3. cost: N voices each holding a note, render time per second of audio
	{
		float l[kBlock], r[kBlock];
		for (int n : { 1, 2, 4, 8 })
		{
			auto pp = std::make_unique<kp::PolySynth>();
			pp->load(argv[1], err);
			pp->load_all();
			pp->set_voices(n);
			pp->program(10);
			for (int b = 0; b < 60; b++) pp->render(l, r, kBlock);
			for (int k = 0; k < n; k++) pp->note_on(48 + 3 * k, 100);
			for (int b = 0; b < 30; b++) pp->render(l, r, kBlock);
			const auto t = std::chrono::steady_clock::now();
			for (int b = 0; b < 300; b++) pp->render(l, r, kBlock);
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
			std::printf("     %d voice%s sounding: %.1f%% of one core\n", n, n > 1 ? "s" : "", 100.0 * s / 3.0);
		}
	}
	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
