// Performance Editor checks on the synth: knob moves give exactly the parameter edits the oracle's
// V55 sends (expectations from reference/wsl/pe_expect.py), PE switches gate them, NRPN switches,
// and a knob position can be restored without applying it.
// Usage: pe_test <data dir> [expectations = data/traces/pe_expect.txt]
#include "control/params.h"
#include "control/perf.h"
#include "synth.h"

#include <cstdio>
#include <fstream>
#include <sstream>
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

void drain(kp::Synth &s)
{
	float l[480], r[480];
	for (int b = 0; b < 20; b++)   // enough for a program burst at the UART rate
		s.render(l, r, 480);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir> [expectations]\n", argv[0]); return 2; }
	const std::string expect = argc > 2 ? argv[2] : "data/traces/pe_expect.txt";
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	check(s.has_perf(), "Performance Editor settings read from ic12.bin");
	int with_perf = 0;
	for (int p = 0; p < s.program_count(); p++)
		with_perf += s.program_perf(p).valid;
	std::printf("     %d of %d programs have Performance Editor settings\n", with_perf, s.program_count());
	check(with_perf == s.program_count(), "every program matched to its factory dump");

	// oracle expectations
	std::ifstream f(expect);
	check(bool(f), ("expectations " + expect).c_str());
	int moves = 0, exact = 0, values = 0;
	std::string line;
	while (std::getline(f, line))
	{
		std::istringstream in(line);
		int p, k, x;
		if (!(in >> p >> k >> x))
			continue;
		std::vector<std::pair<int, int>> want;
		std::string tok;
		while (in >> tok)
		{
			const auto c = tok.find(':');
			want.push_back({ std::stoi(tok.substr(0, c)), std::stoi(tok.substr(c + 1)) });
		}
		s.program(p);
		kp::pe::Edit e[kp::pe::kSets];
		const int n = s.perf_knob(k, x, e);
		bool ok = n == int(want.size());
		for (int i = 0; ok && i < n; i++)
			ok = kp::prm::kDefs[e[i].index].id == want[i].first && (e[i].value & 0xff) == want[i].second;
		moves++;
		values += int(want.size());
		exact += ok;
		if (!ok)
		{
			std::printf("     program %d knob %d x %d: oracle", p, k + 1, x);
			for (auto &w : want) std::printf(" %04x:%d", w.first, w.second);
			std::printf(" | engine");
			for (int i = 0; i < n; i++) std::printf(" %04x:%d", kp::prm::kDefs[e[i].index].id, e[i].value & 0xff);
			std::printf("\n");
		}
		drain(s);
	}
	std::printf("     %d knob moves, %d edits, %d exact\n", moves, values, exact);
	check(moves > 0 && exact == moves, "knob moves match the oracle exactly");

	// PE switches: all off -> no edits; the edit buffer is untouched
	int busy = -1, bk = 0;
	for (int p = 0; p < s.program_count() && busy < 0; p++)
		for (int k = 0; k < kp::pe::kKnobs && busy < 0; k++)
		{
			s.program(p);
			kp::pe::Edit e[kp::pe::kSets];
			if (s.perf_knob(k, 127 - s.perf_knob_value(k), e) > 0) { busy = p; bk = k; }
		}
	check(busy >= 0, "a program with a knob that edits");
	if (busy >= 0)
	{
		s.program(busy);
		for (int set = 0; set < kp::pe::kSets; set++) s.pe_switch(set, false);
		kp::pe::Edit e[kp::pe::kSets];
		check(s.perf_knob(bk, 127 - s.perf_knob_value(bk), e) == 0, "all PE sets off: the knob edits nothing");
		check(!s.pe_switch_on(0), "PE1 off");
		s.midi_cc(99, 0); s.midi_cc(98, 6);   // NRPN 00 06 = PE1
		const auto n1 = s.midi_cc(96, 0);    // data increment: on
		check(n1.number == 6 && n1.step == 1 && s.pe_switch_on(0), "NRPN PE1 increment switches it on");
		s.midi_cc(6, 0);                     // data entry 0: off
		check(!s.pe_switch_on(0), "NRPN PE1 data entry 0 switches it off");
		s.midi_cc(101, 0); s.midi_cc(100, 0);   // RPN selected: data entry no longer reaches PE1
		const auto n2 = s.midi_cc(6, 127);
		check(n2.number == -1 && !s.pe_switch_on(0), "after an RPN selection data entry leaves PE1 alone");
		// the V55 applies nothing when the knob position does not change (A00 at boot: CC19 0 -> no edits)
		s.program(busy);
		for (int set = 0; set < kp::pe::kSets; set++) s.pe_switch(set, true);
		check(s.perf_knob(bk, s.perf_knob_value(bk), e) == 0, "an unchanged knob position applies nothing");
		// Knob Controller Assign: knob 1 on CC20 turns with CC20 (and so does knob 2, still on CC20);
		// CC19 no longer moves it; aftertouch moves its knobs to the first ramp step (127 -> 31)
		{
			s.program(0);
			drain(s);
			s.set_knob_assign(0, 5 + 20);
			kp::Synth::KnobMoves m;
			s.midi_cc(19, 90, &m);
			check(m.count == 0 && s.perf_knob_value(0) != 90, "knob 1 on CC20: CC19 leaves it alone");
			s.midi_cc(20, 64, &m);
			check(m.count == 2 && m.m[0].knob == 0 && m.m[1].knob == 1 && s.perf_knob_value(0) == 64 && s.perf_knob_value(1) == 64,
			      "CC20 turns knob 1 (assigned) and knob 2 (default CC20)");
			s.set_knob_assign(0, 4);   // aftertouch
			s.aftertouch(127, &m);
			check(m.count == 1 && m.m[0].knob == 0 && s.perf_knob_value(0) == 31, "aftertouch knob: first ramp step (127 -> 31)");
			s.aftertouch(0);
			s.set_knob_assign(0, 5 + 19);
			drain(s);
		}
		// Performance Editor Define: set 1 / knob 1 redirected to PE #28 (F1 Cutoff) edits that from the
		// next knob move; a program change restores the stored definition
		{
			s.program(0);
			drain(s);
			const int off = 444;   // set 1, knob 1: PE number
			const int before = s.program_extra(off);
			s.set_program_extra(off, 28);
			kp::pe::Edit e2[kp::pe::kSets];
			const int n = s.perf_knob(0, s.perf_knob_value(0) == 100 ? 20 : 100, e2);
			bool cutoff = false, old = false;
			for (int i = 0; i < n; i++)
			{
				cutoff |= kp::prm::kDefs[e2[i].index].id == kp::prm::kPeAssign[28];
				old |= before && kp::prm::kDefs[e2[i].index].id == kp::prm::kPeAssign[before] && kp::prm::kPeAssign[before] != kp::prm::kPeAssign[28];
			}
			check(cutoff && !old, "PE Define edit: knob 1 now edits PE #28 instead of its stored parameter");
			s.program(0);
			check(s.program_extra(off) == before, "a program change reloads the stored PE Define");
			drain(s);
		}
		// restore a position without applying it
		s.program(busy);
		const int before = s.perf_knob_value(bk);
		s.set_perf_position(bk, 127 - before);
		check(s.perf_knob_value(bk) == 127 - before, "knob position restored without edits");
	}
	drain(s);
	// the knob position that agrees with each program (what the plugin shows): equal to a search
	// through the knob routine itself (from the freshly loaded program, a move to x: the edits it
	// sends), and a position where a move sends nothing whenever there is one. The search runs on
	// an engine that is never rendered (only its edit state matters; its message queue overflows)
	{
		kp::Synth b;
		b.load(argv[1], err);
		int knobs = 0, same = 0, stored_ok = 0, match_ok = 0, exact_any = 0, moved = 0;
		for (int p = 0; p < s.program_count(); p++)
			for (int k = 0; k < kp::pe::kKnobs; k++)
			{
				const auto v = b.program_params(p);
				int best = -1;
				double best_dev = 0;
				bool any = false, at_stored = false;
				const int stored = s.program_perf(p).knob[k];
				for (int x = 0; x < 128; x++)
				{
					b.program(p);
					b.set_perf_position(k, x == 0 ? 1 : 0);
					kp::pe::Edit e[kp::pe::kSets];
					const int n = b.perf_knob(k, x, e);
					double dev = 0;
					for (int i = 0; i < n; i++)
					{
						const auto &d = kp::prm::kDefs[e[i].index];
						dev += double(std::abs(e[i].value - v[size_t(e[i].index)])) / double(d.hi - d.lo + 1);
					}
					any = any || n == 0;
					at_stored = at_stored || (n == 0 && x == stored);
					if (best < 0 || dev < best_dev || (dev == best_dev && std::abs(x - stored) < std::abs(best - stored)))
					{
						best = x;
						best_dev = dev;
					}
				}
				const int m = s.program_knob_match(p, k);
				knobs++;
				same += m == best;
				stored_ok += at_stored;
				exact_any += any;
				moved += m != stored;
				if (any)
				{
					b.program(p);
					b.set_perf_position(k, m == 0 ? 1 : 0);
					kp::pe::Edit e[kp::pe::kSets];
					match_ok += b.perf_knob(k, m, e) == 0;
				}
			}
		std::printf("     %d knobs: the stored position agrees with the program for %d, some position for %d; shown elsewhere: %d\n",
		            knobs, stored_ok, exact_any, moved);
		check(same == knobs, "program_knob_match = a search through the knob routine, every program and knob");
		check(match_ok == exact_any, "where some position agrees with the program, program_knob_match is one (a move there sends nothing)");
	}
	std::printf("%s\n", fails ? "FAILED" : "all checks passed");
	return fails ? 1 : 0;
}
