// Check ported coefficient transforms against oracle-captured param_dispatch calls.
// Usage: coef_test <ic22.bin> <calls.tsv from tools/h8_calls.py>...
// Exit 0 if every call whose compute function is ported matches bit-exactly.
#include "control/coef.h"
#include "control/rom_view.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <ic22.bin> <calls.tsv>...\n", argv[0]);
		return 2;
	}
	kp::ctl::RomView rom;
	if (!rom.load_ic22(argv[1]))
	{
		std::fprintf(stderr, "cannot load %s\n", argv[1]);
		return 2;
	}
	struct Stat { unsigned long n = 0, bad = 0; };
	std::map<std::string, Stat> stats;
	unsigned long skipped = 0;
	int reported = 0;
	for (int fi = 2; fi < argc; fi++)
	{
	std::ifstream f(argv[fi]);
	std::string line;
	std::getline(f, line);   // header
	while (std::getline(f, line))
	{
		std::istringstream ss(line);
		std::string cycle, slot, input, fn, call_in, aux4, aux3, result;
		ss >> cycle >> slot >> input >> fn >> call_in >> aux4 >> aux3 >> result;
		const std::uint32_t in = std::uint32_t(std::strtoul(call_in.c_str(), nullptr, 16));
		const std::uint32_t want = std::uint32_t(std::strtoul(result.c_str(), nullptr, 16));
		bool known = false;
		const std::uint32_t got = kp::ctl::coef_compute(std::uint32_t(std::strtoul(fn.c_str(), nullptr, 16)), in, rom, known);
		if (!known) { skipped++; continue; }
		Stat &s = stats[fn];
		s.n++;
		if (got != want)
		{
			s.bad++;
			if (reported++ < 10)
				std::fprintf(stderr, "MISMATCH fn=%s slot=%s in=%08x want=%08x got=%08x\n", fn.c_str(), slot.c_str(), in, want, got);
		}
	}
	}
	unsigned long bad = 0;
	for (const auto &kv : stats)
	{
		std::printf("fn %s: %lu calls, %lu mismatches\n", kv.first.c_str(), kv.second.n, kv.second.bad);
		bad += kv.second.bad;
	}
	std::printf("not yet ported: %lu calls\n", skipped);
	return bad ? 1 : 0;
}
