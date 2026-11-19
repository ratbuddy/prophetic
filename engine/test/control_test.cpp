// End-to-end control-layer check: translated H8 logic + native scheduler + bit-exact DSP board,
// started from an oracle snapshot (H8 RAM dump + board state at the same frame), fed the oracle's
// UART bytes at their frames. Compares the param_dispatch stream with the oracle's and writes the
// audio for listening.
// Usage: control_test <ic22.bin> <ram_FRAME.bin> <board_state.bin> <calls.bin> <out.wav> [end_frame] [tick_phase]
#include "control/h8ctl.h"
#include "prophecy_board.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> slurp(const std::string &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_wav(const std::string &path, const std::vector<std::int16_t> &lr)
{
	std::ofstream f(path, std::ios::binary);
	auto u32le = [&](std::uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16le = [&](std::uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
	const std::uint32_t bytes = std::uint32_t(lr.size() * 2);
	f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
	u32le(48000); u32le(48000 * 4); u16le(4); u16le(16); f.write("data", 4); u32le(bytes);
	f.write(reinterpret_cast<const char *>(lr.data()), bytes);
}

struct Disp { std::uint64_t frame; std::uint16_t slot; std::uint32_t input; };

} // namespace

int main(int argc, char **argv)
{
	if (argc < 6)
	{
		std::fprintf(stderr, "usage: %s <ic22.bin> <ram_FRAME.bin> <board_state.bin> <calls.bin> <out.wav> [end_frame] [tick_phase]\n", argv[0]);
		return 2;
	}
	const std::string ram_path = argv[2];
	const std::uint64_t start = std::strtoull(ram_path.substr(ram_path.rfind('_') + 1).c_str(), nullptr, 10);
	const std::uint64_t end = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 816800;
	const int phase = argc > 7 ? std::atoi(argv[7]) : 0;

	kp::ctl::RomView rom;
	if (!rom.load_ic22(argv[1])) return 2;
	const auto ram = slurp(ram_path);
	const auto state = slurp(argv[3]);
	kp::ProphecyBoard board;
	if (!board.load_state(state.data(), state.size())) { std::fprintf(stderr, "board state load failed\n"); return 2; }
	if (board.frames() != start) std::fprintf(stderr, "note: board state frame %" PRIu64 " vs RAM dump frame %" PRIu64 "\n", board.frames(), start);
	kp::ctl::H8Control ctl(board);
	if (!ctl.load(rom, ram.data(), ram.size())) { std::fprintf(stderr, "RAM dump load failed\n"); return 2; }

	// Oracle: UART bytes and dispatch calls after the snapshot.
	struct Rx { std::uint64_t frame; std::uint8_t b; };
	std::vector<Rx> rx;
	std::vector<Disp> want;
	{
		const auto raw = slurp(argv[4]);
		for (size_t i = 0; i + 40 <= raw.size(); i += 40)
		{
			std::uint32_t r[10];
			std::memcpy(r, raw.data() + i, 40);
			const std::uint64_t frame = std::uint64_t(r[1]) / 256;
			if (frame < start || frame >= end) continue;
			if (r[0] == 0x01af72) rx.push_back({ frame, std::uint8_t(r[2]) });
			else if (r[0] == 0x018252) want.push_back({ frame, std::uint16_t(r[3]), r[2] });
		}
	}
	std::vector<Disp> got;
	std::uint64_t now = start;
	ctl.on_dispatch = [&](std::uint16_t slot, std::uint32_t in) { got.push_back({ now, slot, in }); };

	std::vector<std::int16_t> wav;
	size_t ri = 0;
	for (now = start; now < end; now++)
	{
		while (ri < rx.size() && rx[ri].frame <= now)
			ctl.uart_byte(rx[ri++].b);
		if ((now - start) % kp::ctl::H8Control::kFramesPerTick == std::uint64_t(phase))
			ctl.tick();
		ctl.pump_dsp();
		std::int32_t l, r;
		board.frame(l, r);
		wav.push_back(std::int16_t(l >> 8));
		wav.push_back(std::int16_t(r >> 8));
	}
	write_wav(argv[5], wav);

	// Compare dispatch streams: in-order prefix, per-slot counts, per-slot last value.
	size_t same = 0;
	while (same < want.size() && same < got.size() && want[same].slot == got[same].slot && want[same].input == got[same].input)
		same++;
	std::map<int, std::pair<int, int>> counts;
	std::map<int, std::pair<std::uint32_t, std::uint32_t>> last;
	for (const auto &d : want) { counts[d.slot].first++; last[d.slot].first = d.input; }
	for (const auto &d : got) { counts[d.slot].second++; last[d.slot].second = d.input; }
	int slots_same_last = 0, slots = 0;
	for (const auto &kv : last) { slots++; slots_same_last += kv.second.first == kv.second.second; }
	std::printf("frames %" PRIu64 "-%" PRIu64 ": uart bytes %zu, oracle dispatches %zu, ours %zu, identical prefix %zu\n",
		start, end, rx.size(), want.size(), got.size(), same);
	std::printf("slots touched %d, same final input %d; dsp writes %" PRIu64 ", program sends %" PRIu64 ", unsupported %" PRIu64 ", dropped %" PRIu64 "\n",
		slots, slots_same_last, ctl.dsp_writes, ctl.program_sends, ctl.unsupported_count, ctl.dropped_writes);
	if (same < want.size() && same < got.size())
		std::printf("first divergence #%zu: oracle frame %" PRIu64 " slot %u in %08x | ours frame %" PRIu64 " slot %u in %08x\n", same,
			want[same].frame, want[same].slot, want[same].input, got[same].frame, got[same].slot, got[same].input);
	int shown = 0;
	for (const auto &kv : counts)
		if (kv.second.first != kv.second.second && shown++ < 12)
			std::printf("  slot %3d: oracle %d calls, ours %d (last %08x vs %08x)\n", kv.first, kv.second.first, kv.second.second,
				last[kv.first].first, last[kv.first].second);
	return 0;
}
