// Korg Prophecy SysEx dumps (Parameter Guide 2-4), for loading program and arpeggio-pattern
// libraries: Current Program Data Dump (40), Program Data Dump (4C: one program, a bank of 64 or all
// 128), Arpeggio Pattern Data Dump (69: one or all 10), All Data Dump (50: global, 128 programs,
// 10 patterns). Header F0 42 3g 41 (g: MIDI channel, any accepted). Data is 7-bit packed: each 7
// bytes become a byte of their top bits (bit k = byte k) followed by their 7 low bits.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kp::syx {

constexpr std::size_t kProgramBytes = 535, kPatternBytes = 128;

struct Program
{
	std::array<std::uint8_t, kProgramBytes> data{};
	int bank = -1, number = -1;   // where the dump says it belongs (-1: the current program)
};

struct Pattern
{
	std::array<std::uint8_t, kPatternBytes> data{};
	int number = 0;   // 0..9 (UP, DOWN, ALT1, ALT2, RANDOM, PAT1-5)
};

struct Contents
{
	std::vector<Program> programs;
	std::vector<Pattern> patterns;
	int messages = 0, skipped = 0;   // dump messages found / not understood
};

std::vector<std::uint8_t> unpack(const std::uint8_t *p, std::size_t n);
std::vector<std::uint8_t> pack(const std::uint8_t *p, std::size_t n);
Contents parse(const std::uint8_t *file, std::size_t n);   // every Prophecy dump message in a file
std::vector<std::uint8_t> current_program_dump(const std::uint8_t *program, int channel = 0);

} // namespace kp::syx
