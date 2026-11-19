// Verify translated H8 code (generated/h8ctl/h8_gen.cpp) against oracle-captured param_dispatch
// calls: run the translated dispatcher on each (slot, input) and compare the hooked
// dsp_cload_write (dsp, coefficient address, value) with what the oracle sent.
// Usage: h8xlat_test <ic22.bin> <calls.tsv>...
#include "control/h8rt.h"
#include "control/rom_view.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace kp::h8;

namespace {

struct Capture : Hooks
{
	bool have = false;
	u32 value = 0;
	u8 dsp = 0, sa = 0;
	std::map<u32, unsigned long> unsupported_counts;
	bool call(u32 addr, Cpu &c) override
	{
		if (addr == 0x019e0a)   // dsp_cload_write: er0 = value, r1l = dsp, r2l = coefficient address
		{
			have = true;
			value = c.er[0];
			dsp = c.r8(9);
			sa = c.r8(10);
			return true;
		}
		return false;
	}
	void unsupported(u32 pc, const char *what) override
	{
		if (unsupported_counts[pc]++ == 0)
			std::fprintf(stderr, "unsupported at %06x: %s\n", pc, what);
	}
};

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <ic22.bin> <calls.tsv>...\n", argv[0]);
		return 2;
	}
	kp::ctl::RomView rom;
	if (!rom.load_ic22(argv[1]))
		return 2;
	Mem mem;
	mem.rom = &rom;
	Capture hooks;
	mem.hooks = &hooks;
	Cpu c;
	c.m = &mem;
	c.hooks = &hooks;
	c.er[7] = 0x05fff0;

	call(c, 0x01a4bc, 0xdead00);   // firmware init of the per-coefficient bounds table (RAM 0x042cc0)

	unsigned long n = 0, bad = 0, missing = 0;
	int reported = 0;
	for (int fi = 2; fi < argc; fi++)
	{
		std::ifstream f(argv[fi]);
		std::string line;
		std::getline(f, line);
		while (std::getline(f, line))
		{
			std::istringstream ss(line);
			std::string cycle, slot, input, fn, call_in, aux4, aux3, result, dsp, sa, sent;
			ss >> cycle >> slot >> input >> fn >> call_in >> aux4 >> aux3 >> result >> dsp >> sa >> sent;
			const u32 sp0 = c.er[7];
			c.er[0] = u32(std::strtoul(input.c_str(), nullptr, 16));
			c.w16(1, u16(std::strtoul(slot.c_str(), nullptr, 10)));
			hooks.have = false;
			call(c, 0x018252, 0xdead00);
			n++;
			if (c.er[7] != sp0)
			{
				std::fprintf(stderr, "stack imbalance after slot %s: %06x -> %06x\n", slot.c_str(), sp0, c.er[7]);
				return 1;
			}
			if (!hooks.have) { missing++; continue; }
			const u32 want = u32(std::strtoul(sent.c_str(), nullptr, 16));
			const u32 wd = u32(std::strtoul(dsp.c_str(), nullptr, 10)), ws = u32(std::strtoul(sa.c_str(), nullptr, 16));
			if (hooks.value != want || hooks.dsp != wd || hooks.sa != ws)
			{
				bad++;
				if (reported++ < 10)
					std::fprintf(stderr, "MISMATCH slot=%s in=%s fn=%s want d%u/%02x/%08x got d%u/%02x/%08x\n", slot.c_str(),
						input.c_str(), fn.c_str(), wd, ws, want, hooks.dsp, hooks.sa, hooks.value);
			}
		}
	}
	std::printf("translated param_dispatch: %lu calls, %lu mismatches, %lu without DSP write, %llu H8 calls executed\n",
		n, bad, missing, (unsigned long long)c.calls);
	return (bad || missing) ? 1 : 0;
}
