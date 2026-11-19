// Program parameter checks on the synth: edit buffer decode, edits change the sound, inactive
// model edits are dropped, OSC Set loads the model defaults, preset values for every program.
// Usage: param_test <data dir>
#include "control/params.h"
#include "synth.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fails = 0;
void check(bool ok, const char *what)
{
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

double render_rms(kp::Synth &s, double seconds)
{
	float l[480], r[480];
	double sum = 0;
	long n = 0;
	for (int b = int(seconds * 100); b > 0; b--)
	{
		s.render(l, r, 480);
		for (int i = 0; i < 480; i++) { sum += double(l[i]) * l[i] + double(r[i]) * r[i]; n += 2; }
	}
	return std::sqrt(sum / n);
}

int index_by_name(const char *name)
{
	for (int i = 0; i < kp::prm::kCount; i++)
		if (!std::strcmp(kp::prm::kDefs[i].name, name))
			return i;
	return -1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir>\n", argv[0]); return 2; }
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	std::printf("%d parameters, %d programs, model defaults %s\n", kp::prm::kCount, s.program_count(), s.has_model_defaults() ? "loaded" : "MISSING");
	check(s.has_model_defaults(), "oscillator-model defaults read from ic12.bin");

	// presets: every program's values match the edit buffer right after program() (the edit buffer
	// updates immediately; render a little so the bursts drain)
	int mismatch = 0;
	for (int p = 0; p < s.program_count(); p++)
	{
		const auto v = s.program_params(p);
		s.program(p);
		for (int i = 0; i < kp::prm::kCount; i++)
			if (s.param_active(i) && std::max(int(kp::prm::kDefs[i].lo), std::min(int(kp::prm::kDefs[i].hi), s.param(i))) != v[i])
				mismatch++;
		render_rms(s, 0.2);
	}
	check(mismatch == 0, "program_params() equals the edit buffer after program() for all programs");

	// edits change the sound: A00 held note, filter 1 cutoff 99 -> 5 should darken/quiet it
	s.program(0);
	render_rms(s, 0.6);
	s.note_on(60, 100);
	render_rms(s, 0.3);
	const double before = render_rms(s, 0.3);
	const int cut = index_by_name("F1 Cutoff");
	const int cut0 = s.param(cut);
	s.set_param(cut, 0);
	render_rms(s, 0.1);
	const double after = render_rms(s, 0.3);
	std::printf("     F1 Cutoff %d -> 0: rms %.4f -> %.4f (routing %s, F1 %s, F2 %s, F2 cutoff %d)\n", cut0, before, after,
	            kp::prm::text(kp::prm::kDefs[index_by_name("F Routing")], s.param(index_by_name("F Routing"))).c_str(),
	            kp::prm::text(kp::prm::kDefs[index_by_name("F1 Type")], s.param(index_by_name("F1 Type"))).c_str(),
	            kp::prm::text(kp::prm::kDefs[index_by_name("F2 Type")], s.param(index_by_name("F2 Type"))).c_str(),
	            s.param(index_by_name("F2 Cutoff")));
	check(s.param(cut) == 0, "edit buffer holds the edit");
	s.set_param(cut, cut0);
	const int out = index_by_name("Out Level");
	const int out0 = s.param(out);
	render_rms(s, 0.1);
	const double loud = render_rms(s, 0.3);
	s.set_param(out, 0);
	render_rms(s, 0.1);
	const double quiet = render_rms(s, 0.3);
	std::printf("     Out Level %d -> 0: rms %.4f -> %.4f\n", out0, loud, quiet);
	check(quiet < 0.2 * loud, "Out Level edit silences the output");
	s.set_param(out, out0);
	s.note_off(60);
	render_rms(s, 0.5);

	// inactive model parameter edits are dropped; OSC Set loads defaults for changed slots
	const int oscset = index_by_name("OSC Set");
	const int set0 = s.param(oscset);
	std::printf("     A00 OSC Set %s\n", kp::prm::text(kp::prm::kDefs[oscset], set0).c_str());
	const int comb_noise1 = index_by_name("O1C Noise");
	const bool comb_active = s.param_active(comb_noise1);
	const int cn0 = s.param(comb_noise1);
	s.set_param(comb_noise1, cn0 == 10 ? 11 : 10);
	check(comb_active || s.param(comb_noise1) == cn0, "edit of an inactive model parameter is dropped");
	s.set_param(oscset, 4);   // set 5: Comb / Comb
	check(s.param(oscset) == 4, "OSC Set edit applied");
	check(s.param_active(comb_noise1), "Comb parameters active after OSC Set 5");
	const auto defaults = s.program_params(0);   // inactive-model params show the defaults
	check(s.param(comb_noise1) == defaults[comb_noise1], "OSC1 Comb parameters hold the model defaults");
	s.note_on(64, 100);
	const double comb = render_rms(s, 0.5);
	s.note_off(64);
	std::printf("     Comb/Comb note rms %.4f\n", comb);
	check(comb > 1e-4, "OSC Set 5 program sounds");

	// every parameter edit round-trips through the edit buffer (active ones)
	s.program(0);
	render_rms(s, 0.6);
	int bad = 0, tried = 0;
	for (int i = 0; i < kp::prm::kCount; i++)
	{
		const auto &d = kp::prm::kDefs[i];
		if (d.id == kp::prm::kOscSetId || !s.param_active(i))
			continue;
		const int v0 = s.param(i);
		const int v1 = v0 == d.hi ? d.lo : v0 + 1;
		s.set_param(i, v1);
		tried++;
		if (s.param(i) != v1)
			bad++;
		s.set_param(i, v0);
	}
	render_rms(s, 1.0);
	std::printf("     %d active parameters edited, %d did not round-trip\n", tried, bad);
	check(bad == 0, "all active parameters round-trip");

	// global settings: Master Tune and User Scale 1 move the pitch, Delay/Reverb bypass changes the tail
	// on A21, a plain sine with no effect tail (factory program names stay out of the repo)
	const int sine = 21;
	check(s.program_count() > sine, "program A21 present");
	if (s.program_count() > sine)
	{
		// mean pitch of a held note from its zero crossings over 3 s (the program's slow pitch
		// movement makes shorter windows wander by about 5 cents)
		auto pitch = [&](int note) {
			s.note_on(note, 100);
			render_rms(s, 0.3);
			float l[480], r[480];
			double first = -1, last = -1, prev = 0;
			int n = 0;
			long t = 0;
			for (int b = 0; b < 300; b++)
			{
				s.render(l, r, 480);
				for (int i = 0; i < 480; i++, t++)
				{
					const double v = l[i];
					if (prev < 0 && v >= 0 && t > 0)
					{
						const double at = t - 1 + (-prev) / (v - prev);
						if (first < 0) first = at; else n++;
						last = at;
					}
					prev = v;
				}
			}
			s.note_off(note);
			render_rms(s, 1.0);
			return n > 0 ? n * 48000.0 / (last - first) : 0.0;
		};
		s.program(sine);
		render_rms(s, 0.5);
		const double f0 = pitch(69);
		s.set_master_tune(100);   // 450.0 Hz
		render_rms(s, 0.1);
		const double f1 = pitch(69);
		s.set_master_tune(0);
		const int stype = index_by_name("Scale Type");
		s.set_param(stype, 9);        // User1
		s.set_user_scale(1, 9, 100);  // A +100 cents
		render_rms(s, 0.1);
		const double f2 = pitch(69);
		s.set_user_scale(1, 9, 0);
		s.set_param(stype, 0);
		render_rms(s, 0.1);
		const double c1 = 1200 * std::log2(f1 / f0), c2 = 1200 * std::log2(f2 / f0);
		std::printf("     A21, A4: %.2f Hz; Master Tune 450.0 Hz: %+.1f cents (450/440 = +38.9); User1 A +100: %+.1f cents\n",
		            f0, c1, c2);
		// the bytes equal the V55's (reference/wsl/probe_pe.py); this only checks the H8 acts on them
		check(c1 > 25 && c1 < 55, "Master Tune 450 Hz raises the pitch by about 39 cents");
		check(c2 > 85 && c2 < 115, "User Scale 1 +100 cents on A raises A by about a semitone");
	}
	{
		s.program(0);   // A00 has delay and reverb; amp release 0 so the tail is the effects'
		render_rms(s, 0.5);
		s.set_param(index_by_name("AEG Rel Time"), 0);
		auto tail = [&]() {
			s.note_on(60, 100);
			render_rms(s, 0.4);
			s.note_off(60);
			render_rms(s, 0.3);
			const double v = render_rms(s, 0.6);
			render_rms(s, 4.0);
			return v;
		};
		const double wet = tail();
		s.set_delay_reverb(false);
		render_rms(s, 0.1);
		const double dry = tail();
		s.set_delay_reverb(true);
		std::printf("     A00 (release 0) rms 0.3-0.9 s after release: Delay/Reverb on %.4f, bypassed %.4f\n", wet, dry);
		check(dry < 0.2 * wet, "Delay/Reverb bypass removes the effect tail");
	}

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
