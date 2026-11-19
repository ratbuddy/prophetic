// Programs: the bursts the engine builds from the program dumps (in your ic12) equal the bursts
// captured from the oracle's V55 (data dir programs/NNN.uart, when present), byte for byte; names
// equal; SysEx dumps pack, unpack and parse back to the same programs.
// Usage: program_test <data dir>
#include "control/sysex.h"
#include "synth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<std::uint8_t> slurp(const std::filesystem::path &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <data dir>\n", argv[0]); return 2; }
	namespace fs = std::filesystem;
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	if (argc > 3 && std::string(argv[2]) == "--burst")   // print a burst (hex): --burst <program> [previous program]
	{
		std::uint8_t prev[kp::syx::kProgramBytes];
		const bool have_prev = argc > 4 && s.program_dump(std::atoi(argv[4]), prev);
		for (auto b : s.program_burst(std::atoi(argv[3]), have_prev ? prev + 435 : nullptr)) std::printf("%02x ", b);
		std::printf("\n");
		return 0;
	}
	if (argc > 3 && std::string(argv[2]) == "--syx")   // write a program as a Current Program Data Dump: --syx <program>
	{
		std::uint8_t d[kp::syx::kProgramBytes];
		if (!s.program_dump(std::atoi(argv[3]), d)) return 1;
		for (auto b : kp::syx::current_program_dump(d)) std::printf("%02x ", b);
		std::printf("\n");
		return 0;
	}
	check(s.program_count() == 128, "128 factory programs");

	// bursts vs the oracle's captures (made by program changes from A00; bank B captures start with
	// the bank select the capture sent, CC0 0 / CC32 1, forwarded as controllers: not the program's)
	int compared = 0, equal = 0, names = 0;
	for (int p = 0; p < s.program_count(); p++)
	{
		char base[16];
		std::snprintf(base, sizeof base, "%03d", p);
		const fs::path up = fs::path(argv[1]) / "programs" / (std::string(base) + ".uart");
		if (!fs::exists(up))
			continue;
		auto want = slurp(up);
		static const std::uint8_t bank_b[8] = { 0x30, 0x10, 0x00, 0x00, 0x30, 0x30, 0x01, 0x03 };
		if (want.size() > 8 && std::memcmp(want.data(), bank_b, 8) == 0)
			want.erase(want.begin(), want.begin() + 8);
		const auto got = s.program_burst(p);
		compared++;
		if (got == want)
			equal++;
		else if (equal + 3 > compared)
		{
			size_t k = 0;
			while (k < got.size() && k < want.size() && got[k] == want[k]) k++;
			std::printf("     program %d: differs at byte %zu of %zu / %zu\n", p, k, got.size(), want.size());
		}
		auto nm = slurp(fs::path(argv[1]) / "programs" / (std::string(base) + ".name"));
		std::string n(nm.begin(), nm.end());
		for (char &ch : n) ch = ch == 0x7f ? '<' : ch;   // shown as the LCD's arrow
		while (!n.empty() && (n.back() == ' ' || n.back() == '\n' || n.back() == '\r')) n.pop_back();
		names += s.program_name(p) == std::string(base) + " " + n;
	}
	std::printf("     %d captured bursts compared, %d equal, %d names equal\n", compared, equal, names);
	if (compared)
	{
		check(equal == compared, "program bursts built from the dumps = the oracle's, byte for byte");
		check(names == compared, "program names = the captured names");
	}

	// SysEx: every factory program through a Current Program Data Dump and back (and as an added
	// program it plays the same burst); bank A as one Program Data Dump of 64
	{
		int round = 0, same_burst = 0;
		std::vector<std::uint8_t> bank;
		const int n0 = s.program_count();
		for (int p = 0; p < n0; p++)
		{
			std::uint8_t d[kp::syx::kProgramBytes];
			if (!s.program_dump(p, d))
				continue;
			const auto m = kp::syx::current_program_dump(d);
			const auto c = kp::syx::parse(m.data(), m.size());
			if (m.size() == 619 && c.programs.size() == 1 && std::memcmp(c.programs[0].data.data(), d, sizeof d) == 0)
				round++;
			const int q = s.add_program(c.programs.empty() ? d : c.programs[0].data.data(), "U");
			same_burst += s.program_burst(q) == s.program_burst(p);
			if (p < 64)
				bank.insert(bank.end(), d, d + sizeof d);
		}
		std::printf("     %d programs: %d through SysEx unchanged, %d added copies play the same burst\n", n0, round, same_burst);
		check(round == n0 && same_burst == n0, "Current Program Data Dump round trip (619 bytes each)");
		std::vector<std::uint8_t> msg = { 0xf0, 0x42, 0x30, 0x41, 0x4c, 0x10, 0x00, 0x00 };   // bank A
		const auto packed = kp::syx::pack(bank.data(), bank.size());
		msg.insert(msg.end(), packed.begin(), packed.end());
		msg.push_back(0xf7);
		const auto c = kp::syx::parse(msg.data(), msg.size());
		bool ok = c.programs.size() == 64;
		for (int k = 0; ok && k < 64; k++)
			ok = c.programs[k].bank == 0 && c.programs[k].number == k &&
			     std::memcmp(c.programs[k].data.data(), &bank[k * kp::syx::kProgramBytes], kp::syx::kProgramBytes) == 0;
		check(ok, "Program Data Dump of bank A: 64 programs back, in order");
	}
	// a User folder: a .syx with one program and an arpeggio pattern (PAT1 with PAT2's steps)
	{
		kp::Synth u;
		if (u.load(argv[1], err))
		{
			const fs::path dir = fs::temp_directory_path() / "kp_user_sysex_test";
			fs::create_directories(dir);
			std::uint8_t d[kp::syx::kProgramBytes];
			u.program_dump(5, d);
			auto file = kp::syx::current_program_dump(d);
			std::vector<std::uint8_t> pat(u.arp_pattern(6), u.arp_pattern(6) + 128);
			const auto pp = kp::syx::pack(pat.data(), pat.size());
			std::vector<std::uint8_t> m = { 0xf0, 0x42, 0x30, 0x41, 0x69, 0x05, 0x00 };   // pattern 5 = PAT1
			m.insert(m.end(), pp.begin(), pp.end());
			m.push_back(0xf7);
			file.insert(file.end(), m.begin(), m.end());
			std::ofstream(dir / "test.syx", std::ios::binary).write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
			std::string report;
			const int n = u.load_user_sysex(dir.string(), &report);
			std::printf("     %s", report.c_str());
			check(n == 1 && u.program_count() == 129 && u.program_name(128).rfind("U001 ", 0) == 0 &&
			      u.program_burst(128) == u.program_burst(5) && std::memcmp(u.arp_pattern(5), pat.data(), 128) == 0,
			      "User/*.syx: the program follows the factory ones (\"" + u.program_name(128) + "\"), the pattern replaces PAT1");
			fs::remove_all(dir);
		}
	}
	// Idle sleep: after a note dies away the engine stops; the next note wakes it, and what it plays
	// equals a continuous run with the silent stretch left out (same emulated frames, same input)
	{
		auto play = [&](kp::Synth &x, bool sleep, std::uint64_t run_frames, std::vector<float> &out) {
			float l[480], r[480];
			x.set_idle_sleep(sleep);
			x.program(21);   // A21, a plain sine: no effect tail
			for (int b = 0; b < 50; b++) x.render(l, r, 480);
			x.note_on(69, 100);
			for (int b = 0; b < 50; b++) x.render(l, r, 480);
			x.note_off(69);
			int asleep_at = -1;
			if (sleep)
				for (int b = 0; b < 2000 && asleep_at < 0; b++) { x.render(l, r, 480); if (x.sleeping()) asleep_at = b; }
			else
				while (x.emulated_frames() < run_frames) x.render(l, r, int(std::min<std::uint64_t>(480, run_frames - x.emulated_frames())));
			if (sleep)
				for (int b = 0; b < 100; b++) x.render(l, r, 480);   // asleep: silence, nothing emulated
			const std::uint64_t at = x.emulated_frames();
			x.note_on(64, 90);
			for (int b = 0; b < 100; b++) { x.render(l, r, 480); out.insert(out.end(), l, l + 480); }
			x.note_off(64);
			return std::make_pair(asleep_at, at);
		};
		kp::Synth a, b;
		std::string e2;
		if (a.load(argv[1], e2) && b.load(argv[1], e2))
		{
			std::vector<float> oa, ob;
			const auto ra = play(a, true, 0, oa);
			play(b, false, ra.second, ob);
			double peak = 0;
			for (float v : oa) peak = std::max(peak, double(std::fabs(v)));
			std::printf("     idle sleep: asleep %.2f s after the release, woke at emulated frame %llu, note peak %.3f\n",
			            ra.first * 0.01, (unsigned long long)ra.second, peak);
			check(ra.first >= 0 && peak > 1e-3 && oa == ob, "idle sleep: sleeps in silence; after waking the output equals a continuous run, sample for sample");
		}
	}
	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
