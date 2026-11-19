// A/B gate for the compiled DSP programs: two synths, same events, one on the interpreter and
// one on dsp_fast; every output sample and every DSP output word must be identical.
//
//   dsp_fast_test <data dir> [programs=128] [--chaos] [--budget]
//
// --chaos randomly marks compiled steps invalid in the fast synth, so execution leaves the
// compiled code mid-frame at arbitrary points (exercises the hand-off to the interpreter).
// --budget caps each frame's instruction budget at a random 1..700 (per 10 ms block, the same for
// both synths), so frames end on the budget mid-program and the next continues where it stopped
// (exercises the budget accounting; the audio is garbage, but must be the same garbage).
#include "dsp_fast.h"
#include "synth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

namespace {

struct Pair
{
	kp::Synth a, b;   // a = interpreter, b = compiled
	std::mt19937 rng{ 1234 };
	bool chaos = false, budget = false;
	long long frames = 0, bad = 0;

	void render(double seconds)
	{
		float al[480], ar[480], bl[480], br[480];
		for (int n = int(seconds * 100); n > 0; n--)
		{
			if (budget)
				kp::dsp_fast_budget_cap(1 + int(rng() % 700u));
			kp::dsp_fast_enable(false);   // a: the interpreter and the board's float stream path
			kp::dsp_fast_stream_raw(false);
			a.render(al, ar, 480);
			kp::dsp_fast_enable(true);
			kp::dsp_fast_stream_raw(true);
			if (chaos)
				for (int d = 0; d < 3; d++)
				{
					auto &f = const_cast<kp::DspFast &>(b.board().fast(d));
					for (int k = 0; k < 4 && f.total_steps > 0; k++)
					{
						const int s = int(rng() % unsigned(f.total_steps));
						f.valid[s] = 0;
						if (f.ipc_of_step[s] >= 0)
							f.step_of_ipc[f.ipc_of_step[s]] = -1;
					}
				}
			b.render(bl, br, 480);
			frames += 480;
			if (std::memcmp(al, bl, sizeof al) || std::memcmp(ar, br, sizeof ar))
			{
				if (bad++ < 5)
					std::printf("  audio mismatch near frame %lld\n", frames);
			}
			for (int d = 0; d < 3; d++)
				if (a.board().outputs(d) != b.board().outputs(d))
				{
					if (bad++ < 5)
						std::printf("  dsp%d output mismatch near frame %lld\n", d + 1, frames);
				}
		}
	}
	template <class F> void both(F f) { f(a); f(b); }
};

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <data dir> [programs] [--chaos] [--budget]\n", argv[0]);
		return 2;
	}
	int programs = 128;
	bool chaos = false, budget = false;
	for (int i = 2; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--chaos")) chaos = true;
		else if (!std::strcmp(argv[i], "--budget")) budget = true;
		else programs = std::atoi(argv[i]);
	}
	std::printf("compiled programs: %d\n", kp::dsp_fast_program_count());
	auto *p = new Pair;
	p->chaos = chaos;
	p->budget = budget;
	std::string err;
	if (!p->a.load(argv[1], err) || !p->b.load(argv[1], err))
	{
		std::fprintf(stderr, "%s\n", err.c_str());
		return 2;
	}
	p->render(0.3);
	{
		// the compiled code's external memory step = the device's, on every st0 transfer mode (on a
		// scratch synth: the test writes random bytes into its delay memory)
		kp::Synth x;
		std::string e2;
		int bad = 0;
		if (x.load(argv[1], e2))
			for (int d = 0; d < 3; d++)
				bad += kp::dsp_fast_xm_selftest(x.board().device(d), 4096, unsigned(d + 1));
		std::printf("external memory step self-test: %d mismatches in 3 x 4096 transfers\n", bad);
		if (bad)
			return 1;
	}
	const int n = std::min(programs, p->a.program_count());
	for (int prog = 0; prog < n; prog++)
	{
		p->both([&](kp::Synth &s) { s.program(prog); });
		p->render(0.5);
		p->both([](kp::Synth &s) { s.note_on(48, 110); s.note_on(55, 70); });
		p->render(0.25);
		p->both([](kp::Synth &s) { s.midi_cc(1, 100); s.pitch_bend(12000); });
		p->render(0.2);
		p->both([](kp::Synth &s) { s.note_off(48); s.note_off(55); s.midi_cc(1, 0); s.pitch_bend(8192); });
		p->render(0.25);
		std::uint64_t fast = 0, interp = 0, deopts = 0;
		for (int d = 0; d < 3; d++)
		{
			fast += p->b.board().fast(d).fast_frames;
			interp += p->b.board().fast(d).interp_frames;
			deopts += p->b.board().fast(d).deopts;
		}
		std::printf("program %3d: %s  (dsp frames: %llu compiled, %llu interpreted, %llu hand-offs)\n", prog, p->bad ? "MISMATCH" : "ok",
		            (unsigned long long)fast, (unsigned long long)interp, (unsigned long long)deopts);
		std::fflush(stdout);
		if (p->bad)
			break;
	}
	std::printf("%s: %lld frames compared, %lld mismatches\n", p->bad ? "FAIL" : "PASS", p->frames, p->bad);
	return p->bad ? 1 : 0;
}
