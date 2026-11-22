// Polyphony (engine/src/poly.h): one voice is the Prophecy exactly (= a plain Synth, sample for
// sample); N voices are N Prophecies, each playing its own note (= the sum of separate Synths fed
// the same program and one note each, sample for sample); a note beyond N takes the voice held
// longest. Also the render cost per voice.
// Usage: poly_test <data dir>
#include "poly.h"
#include "synth.h"

#include <algorithm>
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
	// 3. voices added while a note plays, brought up to date the way the plugin does it (only_from:
	// the program on the new voices only, then voice 0's controllers): the playing voice goes on
	// undisturbed, sample for sample; the new ones take its controllers. Then voices dropped: they
	// fade out over kFadeFrames (no hard cut), the kept voice plays on.
	for (int prog : { 21, 0 })
	{
		const std::string tag = "program " + std::to_string(prog);
		auto make = [&] {
			auto p = std::make_unique<kp::PolySynth>();
			p->load(argv[1], err);
			p->load_all();
			p->program(prog);
			return p;
		};
		auto a = make(), b = make();
		float l[kBlock], r[kBlock];
		auto run = [&](kp::PolySynth &s, int blocks, std::vector<float> *out) {
			for (int k = 0; k < blocks; k++)
			{
				s.render(l, r, kBlock);
				if (out)
					for (int i = 0; i < kBlock; i++) { out->push_back(l[i]); out->push_back(r[i]); }
			}
		};
		for (auto *s : { a.get(), b.get() })
		{
			run(*s, 150, nullptr);   // the program change done
			s->midi_cc(1, 90);         // mod wheel and pitch bend moved, then a held note
			s->pitch_bend(12000);
			s->note_on(60, 100);
			run(*s, 60, nullptr);
		}
		a->set_voices(3);
		a->only_from(1);
		a->program(prog);
		a->match_controllers(1);
		a->only_from(0);
		std::vector<float> oa, ob;
		run(*a, 150, &oa);
		run(*b, 150, &ob);
		double diff = 0;
		for (size_t i = 0; i < oa.size(); i++) diff = std::max(diff, double(std::fabs(oa[i] - ob[i])));
		std::printf("     %s: 1 -> 3 voices during a note: largest output difference %.3g\n", tag.c_str(), diff);
		check(oa == ob, tag + ": voices added during a note: the output is unchanged, sample for sample (the new voices are silent)");
		bool same = true;
		for (int id = 0; id < 128; id++)
			for (int k = 1; k < 3; k++)
				same = same && a->voice(k).controller_value(id) == a->voice(0).controller_value(id);
		check(same, tag + ": the voices added take voice 0's controllers (mod wheel, pitch bend, ...)");

		// dropped: 2 voices, a note on each, then 1 voice; reference = voice 0's note alone
		auto c = std::make_unique<kp::PolySynth>();
		c->load(argv[1], err);
		c->load_all();
		c->set_voices(2);
		c->program(prog);
		kp::Synth s;
		s.load(argv[1], err);
		s.program(prog);
		float sl[kBlock], sr[kBlock];
		std::vector<float> oc, os;
		auto both = [&](int blocks) {
			for (int k = 0; k < blocks; k++)
			{
				c->render(l, r, kBlock);
				s.render(sl, sr, kBlock);
				for (int i = 0; i < kBlock; i++) { oc.push_back(l[i]); os.push_back(sl[i]); }
			}
		};
		both(150);
		c->note_on(60, 100);   // voice 0
		s.note_on(60, 100);
		c->note_on(67, 100);   // voice 1
		both(60);
		const size_t at = oc.size();
		c->set_voices(1);
		both(10);
		std::vector<double> d(oc.size());
		for (size_t i = 0; i < oc.size(); i++) d[i] = double(oc[i]) - os[i];   // voice 1's part
		double step = 0, before = 0, rest = 0;
		for (size_t i = at - 4800; i < at; i++) step = std::max(step, std::fabs(d[i] - d[i - 1]));
		for (size_t i = at - 480; i < at; i++) before = std::max(before, std::fabs(d[i]));
		for (size_t i = at + kp::PolySynth::kFadeFrames; i < d.size(); i++) rest = std::max(rest, std::fabs(d[i]));
		const double jump = std::fabs(d[at] - d[at - 1]);
		std::printf("     %s: 2 -> 1 voices: dropped voice level %.4f, step at the drop %.4f (its largest step before %.4f), after the fade %.3g\n",
		            tag.c_str(), before, jump, step, rest);
		check(before > 1e-3 && jump <= step && rest == 0.0, tag + ": a dropped voice fades out over 20 ms (no click), then is silent; the kept voice plays on");
	}
	// 4. idle sleep: a silent engine stays asleep through a mod wheel sweep, a pitch bend and a Perf
	// Knob move (with several voices, every voice gets them), and its next note sounds exactly as
	// if only the final values had been sent together with that note
	for (int prog : { 21, 0 })
	{
		const std::string tag = "program " + std::to_string(prog);
		kp::Synth a, b;
		float l[kBlock], r[kBlock];
		for (kp::Synth *s : { &a, &b })
		{
			s->load(argv[1], err);
			s->set_idle_sleep(true);
			s->program(prog);
			for (int k = 0; k < 300; k++) s->render(l, r, kBlock);   // program done, then 1 s of silence
		}
		const bool slept = a.sleeping() && b.sleeping();
		kp::pe::Edit e[kp::pe::kSets];
		for (int v = 0; v <= 127; v++) a.midi_cc(1, v);
		a.pitch_bend(12000);
		a.perf_knob(0, 100, e);
		bool stayed = true, silent = true;
		for (int k = 0; k < 50; k++)
		{
			a.render(l, r, kBlock);
			b.render(l, r, kBlock);
			stayed = stayed && a.sleeping();
			for (int i = 0; i < kBlock; i++) silent = silent && l[i] == 0.0f;
		}
		check(slept && stayed && silent, tag + ": a sleeping engine stays asleep through a mod wheel sweep, a bend and a Perf Knob move");
		a.note_on(60, 100);
		b.midi_cc(1, 127);   // the final values, sent with the note
		b.pitch_bend(12000);
		b.perf_knob(0, 100, e);
		b.note_on(60, 100);
		std::vector<float> oa, ob;
		for (int k = 0; k < 100; k++)
		{
			a.render(l, r, kBlock);
			oa.insert(oa.end(), l, l + kBlock);
			b.render(l, r, kBlock);
			ob.insert(ob.end(), l, l + kBlock);
		}
		double peak = 0;
		for (float v : oa) peak = std::max(peak, double(std::fabs(v)));
		check(peak > 1e-3 && oa == ob && a.controller_value(0x11) == b.controller_value(0x11),
		      tag + ": its next note = the final values sent with the note, sample for sample (nothing lost)");
	}
	// 5. cost: N voices each holding a note, render time per second of audio
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
