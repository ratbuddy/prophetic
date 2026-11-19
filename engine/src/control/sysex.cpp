#include "control/sysex.h"

#include <cstring>

namespace kp::syx {

std::vector<std::uint8_t> unpack(const std::uint8_t *p, std::size_t n)
{
	std::vector<std::uint8_t> out;
	out.reserve(n * 7 / 8 + 7);
	for (std::size_t i = 0; i < n; i += 8)
	{
		const std::uint8_t top = p[i];
		for (std::size_t k = 1; k < 8 && i + k < n; k++)
			out.push_back(std::uint8_t((p[i + k] & 0x7f) | (((top >> (k - 1)) & 1) << 7)));
	}
	return out;
}

std::vector<std::uint8_t> pack(const std::uint8_t *p, std::size_t n)
{
	std::vector<std::uint8_t> out;
	out.reserve(n * 8 / 7 + 8);
	for (std::size_t i = 0; i < n; i += 7)
	{
		std::uint8_t top = 0;
		const std::size_t m = n - i < 7 ? n - i : 7;
		for (std::size_t k = 0; k < m; k++)
			top |= std::uint8_t(((p[i + k] >> 7) & 1) << k);
		out.push_back(top);
		for (std::size_t k = 0; k < m; k++)
			out.push_back(p[i + k] & 0x7f);
	}
	return out;
}

namespace {

void add_programs(Contents &c, const std::vector<std::uint8_t> &d, std::size_t at, int count, int bank, int number)
{
	for (int k = 0; k < count && at + (k + 1) * kProgramBytes <= d.size(); k++)
	{
		Program p;
		std::memcpy(p.data.data(), &d[at + k * kProgramBytes], kProgramBytes);
		p.bank = bank < 0 ? -1 : (count == 128 ? k / 64 : bank);
		p.number = number < 0 ? (bank < 0 ? -1 : k % 64) : number;
		c.programs.push_back(p);
	}
}

void add_patterns(Contents &c, const std::vector<std::uint8_t> &d, std::size_t at, int count, int first)
{
	for (int k = 0; k < count && at + (k + 1) * kPatternBytes <= d.size(); k++)
	{
		Pattern p;
		std::memcpy(p.data.data(), &d[at + k * kPatternBytes], kPatternBytes);
		p.number = first + k;
		c.patterns.push_back(p);
	}
}

} // namespace

Contents parse(const std::uint8_t *f, std::size_t n)
{
	Contents c;
	for (std::size_t i = 0; i + 5 < n; i++)
	{
		if (f[i] != 0xf0 || f[i + 1] != 0x42 || (f[i + 2] & 0xf0) != 0x30 || f[i + 3] != 0x41)
			continue;
		std::size_t e = i + 4;
		while (e < n && f[e] < 0x80)
			e++;
		if (e >= n)
			break;
		if (f[e] != 0xf7)   // cut short by another message: skip it
		{
			i = e - 1;
			continue;
		}
		const std::uint8_t fn = f[i + 4];
		const std::uint8_t *b = f + i + 5;   // after the function byte
		const std::size_t len = e - (i + 5);
		c.messages++;
		const std::size_t before = c.programs.size() + c.patterns.size();
		if (fn == 0x40 && len > 1)   // current program: type byte, data
		{
			const auto d = unpack(b + 1, len - 1);
			add_programs(c, d, 0, 1, -1, -1);
		}
		else if (fn == 0x4c && len > 3)   // program / bank / all: unit|bank, number, 00, data
		{
			const int unit = (b[0] >> 4) & 3, bank = b[0] & 1, number = b[1] & 0x3f;
			const auto d = unpack(b + 3, len - 3);
			if (unit == 0) add_programs(c, d, 0, 1, bank, number);
			else if (unit == 1) add_programs(c, d, 0, 64, bank, -1);
			else add_programs(c, d, 0, 128, 0, -1);
		}
		else if (fn == 0x69 && len > 2)   // arpeggio pattern(s): unit|number, 00, data
		{
			const auto d = unpack(b + 2, len - 2);
			if ((b[0] >> 4) & 1) add_patterns(c, d, 0, 10, 0);
			else add_patterns(c, d, 0, 1, b[0] & 0x0f);
		}
		else if (fn == 0x50 && len > 2)   // all data: 00, 00, global, 128 programs, 10 patterns
		{
			const auto d = unpack(b + 2, len - 2);
			const std::size_t tail = 128 * kProgramBytes + 10 * kPatternBytes;
			if (d.size() >= tail)
			{
				add_programs(c, d, d.size() - tail, 128, 0, -1);
				add_patterns(c, d, d.size() - 10 * kPatternBytes, 10, 0);
			}
		}
		if (c.programs.size() + c.patterns.size() == before)
			c.skipped++;
		i = e;
	}
	return c;
}

std::vector<std::uint8_t> current_program_dump(const std::uint8_t *program, int channel)
{
	std::vector<std::uint8_t> m = { 0xf0, 0x42, std::uint8_t(0x30 | (channel & 15)), 0x41, 0x40, 0x01 };
	const auto d = pack(program, kProgramBytes);
	m.insert(m.end(), d.begin(), d.end());
	m.push_back(0xf7);
	return m;
}

} // namespace kp::syx
