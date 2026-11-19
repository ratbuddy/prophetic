// Arpeggiator: pattern orders, octaves, sort, latch, gate and step timing; with a data dir, every
// scenario captured from the oracle (reference/wsl/arp_expect.py).
// Usage: arp_test [<data dir> [expectations = data/traces/arp_expect.txt]]
#include "arp.h"
#include "synth.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

// note-ons produced over `steps` steps (free-running, 48 kHz, 120 bpm, 1/16 = 6000 frames)
std::vector<int> run(kp::Arp &a, const kp::Arp::Settings &s, int steps, std::vector<long> *times = nullptr)
{
	std::vector<int> out;
	long t = 0;
	while (int(out.size()) < steps && t < 48000L * 60)
	{
		a.run(s, 100, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &e) {
			if (e.on)
			{
				out.push_back(e.note);
				if (times) times->push_back(t);
			}
		});
		t += 100;
	}
	return out;
}

std::string str(const std::vector<int> &v)
{
	std::string s;
	for (int n : v) s += std::to_string(n) + " ";
	return s;
}

} // namespace

// The oracle's V55 playing each scenario of reference/wsl/arp_expect.py: our arpeggio (patterns from
// the user's ic12 via the synth) must contain the captured note sequence: notes and velocities
// exactly, gates within 6 % of a step, onsets within 0.08 step (the oracle's UART timing jitter).
struct Ev { int note, vel; double gate, onset; };

std::vector<Ev> parse_seq(std::istringstream &in)
{
	std::vector<Ev> v;
	std::string tok;
	while (in >> tok)
	{
		Ev e{};
		if (std::sscanf(tok.c_str(), "%d:%d:%lf:%lf", &e.note, &e.vel, &e.gate, &e.onset) == 4)
			v.push_back(e);
	}
	return v;
}

void oracle(const char *data, const char *expect)
{
	kp::Synth syn;
	std::string err;
	if (!syn.load(data, err)) { check(false, "load " + std::string(data) + ": " + err); return; }
	check(syn.arp_pattern(0) != nullptr, "arpeggio patterns read from ic12.bin");
	std::ifstream f(expect);
	check(bool(f), std::string("expectations ") + expect);
	std::string line;
	int scen = 0, ok = 0;
	while (std::getline(f, line))
	{
		const auto bar = line.find(" | ");
		if (bar == std::string::npos || !syn.arp_pattern(0))
			continue;
		std::istringstream head(line.substr(0, bar)), tail(line.substr(bar + 3));
		std::string label, kv;
		head >> label;
		int pattern = 0, oct = 1, cc1 = 0;
		std::vector<std::pair<int, int>> notes, edits, globals;
		while (head >> kv)
		{
			const auto eq = kv.find('=');
			const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == "pattern") pattern = std::stoi(v);
			else if (k == "oct") oct = std::stoi(v);
			else if (k == "cc1") cc1 = v == "-" ? 0 : std::stoi(v);
			else if (v != "-")
			{
				std::istringstream items(v);
				std::string it;
				while (std::getline(items, it, ','))
				{
					const auto c = it.find(':');
					(k == "notes" ? notes : k == "globals" ? globals : edits).push_back({ std::stoi(it.substr(0, c)), std::stoi(it.substr(c + 1)) });
				}
			}
		}
		const auto want = parse_seq(tail);
		// settings as the Prophecy has them for that pattern, then the scenario's edits
		kp::Arp::PatternData pd = kp::Arp::PatternData::from_bytes(syn.arp_pattern(pattern));
		for (auto &e : edits)
		{
			const int id = e.first, v = e.second;
			if (id == 18) pd.step = v;
			else if (id == 19) pd.sort = v != 0;
			else if (id == 22) pd.velocity = v == 128 ? kp::Arp::kVelKey : (v == 129 ? kp::Arp::kVelStep : v);
			else if (id == 25) pd.gate = v;
			else if (id == 28) pd.type = v;
			else if (id == 29) pd.oct_alt = v;
			else if (id == 24) pd.vel_int = v;
			else if (id == 27) pd.gate_int = v;
			else if (id >= 33 && id < 33 + 4 * kp::Arp::kSteps && (id - 33) % 4 == 0) pd.steps[(id - 33) / 4].offset = v;
		}
		kp::Arp::Settings s;
		s.on = true;
		s.pattern = pattern;
		s.octaves = oct;
		s.step = pd.step;
		s.sort = pd.sort;
		s.velocity = pd.velocity;
		s.gate = pd.gate;
		s.user = &pd;
		s.vel_int = pd.vel_int;
		s.gate_int = pd.gate_int;
		for (auto &g : globals)   // Arpeggiator Velocity / Gate Controller = CC#1 (4): the mod wheel's value
		{
			if (g.first == 177 && g.second == 4) s.vel_ctrl = cc1 * 32767 / 127;
			if (g.first == 178 && g.second == 4) s.gate_ctrl = cc1 * 32767 / 127;
		}
		kp::Arp a;
		for (auto &n : notes) a.note_on(s, n.first, n.second);
		// our arpeggio: 1/8 at 120 bpm (or 1/16) = 12000 (6000) frames per step, 32-frame blocks
		const double step_frames = (pd.step == kp::Arp::k16 ? 6000.0 : 12000.0);
		std::vector<Ev> got;
		std::vector<std::pair<long, kp::Arp::Out>> evs;
		for (long t = 0; t < long(step_frames * 120); t += 32)
			a.run(s, 32, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &e) { evs.push_back({ t, e }); });
		for (size_t i = 0; i < evs.size(); i++)
			if (evs[i].second.on)
				for (size_t j = i + 1; j < evs.size(); j++)
					if (!evs[j].second.on && evs[j].second.note == evs[i].second.note)
					{
						got.push_back({ evs[i].second.note, evs[i].second.velocity, (evs[j].first - evs[i].first) / step_frames * 100,
						                evs[i].first / step_frames });
						break;
					}
		// find the oracle's run in ours
		bool found = false;
		for (size_t st = 0; st + want.size() <= got.size() && !found; st++)
		{
			bool m = true;
			for (size_t k = 0; k < want.size() && m; k++)
			{
				const Ev &w = want[k], &g = got[st + k];
				m = w.note == g.note && w.vel == g.vel && std::fabs(w.gate - g.gate) <= 6 &&
				    std::fabs(w.onset - (g.onset - got[st].onset)) <= 0.08;
			}
			found = m;
		}
		scen++;
		ok += found;
		if (!found)
		{
			std::printf("     %s: oracle", label.c_str());
			for (size_t k = 0; k < want.size() && k < 12; k++) std::printf(" %d/%d/%.0f@%.2f", want[k].note, want[k].vel, want[k].gate, want[k].onset);
			std::printf("\n     %*s  ours  ", int(label.size()), "");
			for (size_t k = 0; k < got.size() && k < 12; k++) std::printf(" %d/%d/%.0f@%.2f", got[k].note, got[k].vel, got[k].gate, got[k].onset);
			std::printf("\n");
		}
	}
	std::printf("     %d scenarios, %d match the oracle\n", scen, ok);
	check(scen > 0 && ok == scen, "arpeggio = the oracle's V55 in every scenario");
}

int main(int argc, char **argv)
{
	kp::Arp::Settings s;
	s.on = true;
	auto pattern = [&](int pat, int oct, bool sort, std::vector<int> keys, int steps) {
		kp::Arp a;
		s.pattern = pat;
		s.octaves = oct;
		s.sort = sort;
		for (int k : keys) a.note_on(s, k, 90);
		return run(a, s, steps);
	};
	const std::vector<int> chord = { 64, 60, 67 };   // played E, C, G
	auto up = pattern(kp::Arp::kUp, 1, true, chord, 6);
	check(str(up) == "60 64 67 60 64 67 ", "UP sorted: " + str(up));
	auto played = pattern(kp::Arp::kUp, 1, false, chord, 6);
	check(str(played) == "64 60 67 64 60 67 ", "UP as played: " + str(played));
	auto down = pattern(kp::Arp::kDown, 2, true, chord, 6);
	check(str(down) == "79 76 72 67 64 60 ", "DOWN 2 octaves: " + str(down));
	auto alt1 = pattern(kp::Arp::kAlt1, 1, true, chord, 8);
	check(str(alt1) == "60 64 67 64 60 64 67 64 ", "ALT1: " + str(alt1));
	auto alt2 = pattern(kp::Arp::kAlt2, 1, true, chord, 8);
	check(str(alt2) == "60 64 67 67 64 60 60 64 ", "ALT2: " + str(alt2));
	auto up2 = pattern(kp::Arp::kUp, 2, true, chord, 7);
	check(str(up2) == "60 64 67 72 76 79 60 ", "UP 2 octaves: " + str(up2));

	// timing: 1/16 at 120 bpm = 6000 frames per step at 48 kHz
	{
		kp::Arp a;
		s.pattern = kp::Arp::kUp;
		s.octaves = 1;
		s.step = kp::Arp::k16;
		a.note_on(s, 60, 100);
		std::vector<long> t;
		run(a, s, 5, &t);
		bool even = t.size() == 5;
		for (size_t i = 1; i < t.size(); i++)
			even = even && std::abs(t[i] - t[i - 1] - 6000) <= 100;
		check(even, "free-running 1/16 at 120 bpm: steps 6000 frames apart");
	}
	// latch: releasing keeps the arpeggio; a new chord replaces it
	{
		kp::Arp a;
		s.latch = true;
		a.note_on(s, 60, 100);
		a.note_on(s, 64, 100);
		a.note_off(s, 60);
		a.note_off(s, 64);
		auto v = run(a, s, 4);
		check(str(v) == "60 64 60 64 ", "latch keeps released notes: " + str(v));
		a.note_on(s, 70, 100);
		auto w = run(a, s, 2);
		check(str(w) == "70 70 ", "new chord replaces the latched one: " + str(w));
		s.latch = false;
	}
	// gate: 50% closes the note half way through the step
	{
		kp::Arp a;
		s.gate = 50;
		a.note_on(s, 60, 100);
		long on = -1, off = -1, t = 0;
		for (int b = 0; b < 100 && off < 0; b++, t += 100)
			a.run(s, 100, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &e) { (e.on ? on : off) = t; });
		check(on >= 0 && off - on >= 2900 && off - on <= 3100, "gate 50%: note off half a step after note on");
		s.gate = 80;
	}
	// host transport: steps on the 1/16 grid
	{
		kp::Arp a;
		a.note_on(s, 60, 100);
		int fired = 0;
		double ppq = 0;
		for (int b = 0; b < 480; b++)   // 480 blocks of 100 frames = 1 s = 2 beats at 120 bpm
		{
			a.run(s, 100, 48000.0, 120.0, ppq, [&](const kp::Arp::Out &e) { if (e.on) fired++; });
			ppq += 100.0 / 48000.0 * 2.0;
		}
		check(fired == 8 || fired == 9, "host-locked: 8 sixteenths in 2 beats (got " + std::to_string(fired) + ")");
	}
	// legato: at 100% gate a step on the sounding note extends it; note-offs carry the step velocity
	{
		kp::Arp a;
		kp::Arp::Settings t = s;
		t.gate = 100;
		t.step = kp::Arp::k16;
		a.note_on(t, 60, 90);
		int ons = 0, offs = 0, last_off_vel = -1;
		long tt = 0;
		for (int b = 0; b < 400; b++, tt += 100)
			a.run(t, 100, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &e) { if (e.on) ons++; else { offs++; last_off_vel = e.velocity; } });
		a.stop([&](const kp::Arp::Out &e) { if (!e.on) { offs++; last_off_vel = e.velocity; } });
		check(ons == 1 && offs == 1 && last_off_vel == 90, "100% gate on one note: one long note, note-off velocity 90 (ons " +
		      std::to_string(ons) + ", offs " + std::to_string(offs) + ")");
	}
	// Key Sync (measured on the oracle): Off = the first step waits for the running clock's next step,
	// On = it sounds at the key
	for (bool ks : { false, true })
	{
		kp::Arp a;
		kp::Arp::Settings t = s;
		t.key_sync = ks;
		t.step = kp::Arp::k16;   // 6000 frames per step
		t.gate = 80;
		long first = -1, tt = 0;
		for (int b = 0; b < 30; b++, tt += 100)   // the clock runs 3000 frames (half a step) without notes
			a.run(t, 100, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &) {});
		a.note_on(t, 60, 100);
		const long pressed = tt;
		for (int b = 0; b < 200 && first < 0; b++, tt += 100)
			a.run(t, 100, 48000.0, 120.0, -1.0, [&](const kp::Arp::Out &e) { if (e.on && first < 0) first = tt; });
		const long lag = first - pressed;
		check(ks ? lag <= 100 : (lag >= 2900 && lag <= 3100),
		      std::string("Key Sync ") + (ks ? "On: first step at the key" : "Off: first step at the clock's next step") +
		          " (lag " + std::to_string(lag) + " frames)");
	}
	if (argc > 1)
		oracle(argv[1], argc > 2 ? argv[2] : "data/traces/arp_expect.txt");
	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
