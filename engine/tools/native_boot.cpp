// Boot the Prophecy firmware natively (the H8's reset handler, then the V55's power-on messages,
// on to Synth::kBootFrames) and write the engine snapshot the plugin loads: state.bin (the DSP
// board) and ram.bin (the H8 RAM). From the user's ROMs alone: no emulator.
//
// Usage: native_boot <data dir with ic22.bin + ic12.bin> <out dir> [--compare <state.bin> <ram.bin>]
// --compare: byte differences against another snapshot (an oracle capture), by region.
// The output is firmware-derived: write it under data/ or out/, never commit it.
#include "prophecy_board.h"
#include "synth.h"

#include "emu.h"
#define private public   // the comparison reads the cores' memories (as prophecy_board.cpp does)
#define protected public
#include "tms57002.h"
#undef private
#undef protected

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> slurp(const std::string &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void diff(const char *what, const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b, std::size_t base, std::size_t len,
          std::size_t chunk)
{
	std::size_t n = 0, first = ~std::size_t(0);
	for (std::size_t i = 0; i < len && base + i < a.size() && base + i < b.size(); i++)
		if (a[base + i] != b[base + i])
		{
			n++;
			if (first == ~std::size_t(0))
				first = i;
		}
	std::printf("  %-28s %8zu bytes, %6zu differ", what, len, n);
	if (n)
		std::printf(" (first at +0x%zx)", first);
	std::printf("\n");
	if (n && chunk)   // differing ranges, chunk by chunk
	{
		int shown = 0;
		for (std::size_t c = 0; c < len && shown < 24; c += chunk)
		{
			std::size_t m = 0;
			for (std::size_t i = c; i < c + chunk && i < len; i++)
				m += a[base + i] != b[base + i];
			if (m)
			{
				std::printf("      +0x%05zx: %zu\n", c, m);
				shown++;
			}
		}
	}
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <data dir> <out dir> [--compare <state.bin> <ram.bin>]\n", argv[0]);
		return 2;
	}
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err, true))
	{
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const auto state = s.board().save_state();
	const auto ram = s.h8_ram();
	namespace fs = std::filesystem;
	fs::create_directories(argv[2]);
	std::ofstream(fs::path(argv[2]) / "state.bin", std::ios::binary).write(reinterpret_cast<const char *>(state.data()), std::streamsize(state.size()));
	std::ofstream(fs::path(argv[2]) / "ram.bin", std::ios::binary).write(reinterpret_cast<const char *>(ram.data()), std::streamsize(ram.size()));
	std::printf("native boot: %llu frames; wrote state.bin (%zu bytes), ram.bin (%zu bytes) to %s\n",
	            (unsigned long long)kp::Synth::kBootFrames, state.size(), ram.size(), argv[2]);
	if (argc >= 6 && std::string(argv[3]) == "--compare")
	{
		const auto os = slurp(argv[4]), orr = slurp(argv[5]);
		std::printf("against %s, %s:\n", argv[4], argv[5]);
		diff("H8 SRAM 0x40000-0x5FFFF", ram, orr, 0, 0x20000, 0x1000);
		diff("H8 on-chip RAM 0xFFD10-", ram, orr, 0x20000, 0x200, 0);
		diff("board state", state, os, 0, std::min(state.size(), os.size()), 0);
		kp::ProphecyBoard a, b;
		if (a.load_state(state.data(), state.size()) && b.load_state(os.data(), os.size()))
			for (int d = 0; d < 3; d++)
			{
				auto &x = a.device(d), &y = b.device(d);
				int prog = 0, cm = 0, d0 = 0, d1 = 0;
				for (int k = 0; k < 256; k++)
				{
					prog += x.program->read_dword(k) != y.program->read_dword(k);
					cm += x.cmem[k] != y.cmem[k];
					d0 += x.dmem0[k] != y.dmem0[k];
				}
				for (int k = 0; k < 32; k++)
					d1 += x.dmem1[k] != y.dmem1[k];
				const auto &xm = x.data->bytes(), &ym = y.data->bytes();
				std::size_t xd = 0, xnz = 0, ynz = 0;
				for (std::size_t k = 0; k < xm.size() && k < ym.size(); k++)
				{
					xd += xm[k] != ym[k];
					xnz += xm[k] != 0;
					ynz += ym[k] != 0;
				}
				std::printf("  dsp%d: program %d/256, cmem %d/256, dmem0 %d/256, dmem1 %d/32 words differ; delay RAM %zu/%zu bytes"
				            " differ (non-zero: ours %zu, theirs %zu); st0 %06x/%06x st1 %06x/%06x pc %02x/%02x ba0 %02x/%02x xba %05x/%05x\n",
				            d + 1, prog, cm, d0, d1, xd, xm.size(), xnz, ynz, unsigned(x.st0), unsigned(y.st0), unsigned(x.st1), unsigned(y.st1),
				            x.pc, y.pc, x.ba0, y.ba0, unsigned(x.xba), unsigned(y.xba));
			}
	}
	return 0;
}
