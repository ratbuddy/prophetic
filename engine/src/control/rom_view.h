// CPU-order view of the user's H8 ROM (ic22). The dump is word-swapped relative to the CPU
// (MAME ROM_LOAD16_WORD_SWAP); load() un-swaps it. Reads beyond the image return 0xff.
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace kp::ctl {

class RomView
{
public:
	bool load_ic22(const std::string &path)
	{
		std::ifstream f(path, std::ios::binary);
		std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (raw.size() < 2)
			return false;
		m.assign(raw.size(), 0);
		for (std::size_t i = 0; i + 1 < raw.size(); i += 2)
		{
			m[i] = raw[i + 1];
			m[i + 1] = raw[i];
		}
		return true;
	}
	std::uint8_t r8(std::uint32_t a) const { return a < m.size() ? m[a] : 0xff; }
	std::uint16_t r16(std::uint32_t a) const { return std::uint16_t((r8(a) << 8) | r8(a + 1)); }
	std::uint32_t r32(std::uint32_t a) const { return (std::uint32_t(r16(a)) << 16) | r16(a + 2); }
	std::size_t size() const { return m.size(); }

private:
	std::vector<std::uint8_t> m;
};

} // namespace kp::ctl
