// Coefficient memory after parameter edits, for comparison with the oracle (the same edits sent
// as SysEx PARAMETER CHANGE, reference/wsl/probe_edit_cmem.sh): start from the boot snapshot,
// render `pre` seconds, apply the edits, render `post` seconds, write the three DSPs' 256
// coefficient words (3 x 256 u32).
// Usage: synth_cmem_edits <data dir> <pre s> <post s> <out.bin> [<param id>=<raw value> ...]
#include "control/params.h"
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
	if (argc < 5) { std::fprintf(stderr, "usage: %s <data dir> <pre s> <post s> <out.bin> [id=value ...]\n", argv[0]); return 2; }
	kp::Synth s;
	std::string err;
	if (!s.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
	float l[480], r[480];
	auto run = [&](double sec) { for (int n = int(sec * 100); n > 0; n--) s.render(l, r, 480); };
	run(std::atof(argv[2]));
	for (int a = 5; a < argc; a++)
	{
		const std::string e = argv[a];
		const auto eq = e.find('=');
		const int id = std::atoi(e.substr(0, eq).c_str()), v = std::atoi(e.substr(eq + 1).c_str());
		const int i = kp::prm::index_of(id);
		if (i < 0) { std::fprintf(stderr, "unknown parameter id %d\n", id); return 2; }
		s.set_param(i, v);
	}
	run(std::atof(argv[3]));
	std::ofstream o(argv[4], std::ios::binary);
	for (int d = 0; d < 3; d++)
		o.write(reinterpret_cast<const char *>(s.board().device(d).cmem), sizeof s.board().device(d).cmem);
	return 0;
}
