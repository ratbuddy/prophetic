// Coefficient memory after a program change, for comparison with the oracle (engine/test/replay
// with a cmem dump): load the program, render, write the three DSPs' 256 coefficient words.
// Usage: synth_cmem <data dir> <program> <seconds> <out.bin>   (out: 3 x 256 u32, DSP1..3)
#include "prophecy_board.h"
#include "synth.h"

#include "emu.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define private public
#define protected public
#include "tms57002.h"
#undef private
#undef protected

int main(int argc, char **argv)
{
	if (argc < 5) { std::fprintf(stderr, "usage: %s <data dir> <program> <seconds> <out.bin>\n", argv[0]); return 2; }
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	s.program(std::atoi(argv[2]));
	float l[480], r[480];
	for (int n = int(std::atof(argv[3]) * 100); n > 0; n--)
		s.render(l, r, 480);
	std::ofstream o(argv[4], std::ios::binary);
	for (int d = 0; d < 3; d++)
		o.write(reinterpret_cast<const char *>(s.board().device(d).cmem), sizeof s.board().device(d).cmem);
	return 0;
}
