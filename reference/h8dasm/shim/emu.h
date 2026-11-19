// Minimal stand-in for MAME's emu.h so the BSD-3 H8 disassembler (h8d.cpp / h8hd.cpp from the
// MAME checkout) builds as a standalone tool. Ours; contains no MAME code.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using offs_t = u32;

namespace util {

inline void stream_format(std::ostream &os, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	os << buf;
}

class disasm_interface
{
public:
	enum : u32 {
		LENGTHMASK = 0x0000ffff,
		STEP_OVER  = 0x00010000,
		STEP_OUT   = 0x00020000,
		STEP_COND  = 0x00040000,
		SUPPORTED  = 0x80000000
	};

	// Big-endian view of the CPU address space (the H8 is big-endian).
	class data_buffer
	{
	public:
		explicit data_buffer(const std::vector<u8> &mem) : m(mem) {}
		u8 r8(offs_t a) const { return a < m.size() ? m[a] : 0xff; }
		u16 r16(offs_t a) const { return u16((r8(a) << 8) | r8(a + 1)); }
		u32 r32(offs_t a) const { return (u32(r16(a)) << 16) | r16(a + 2); }
	private:
		const std::vector<u8> &m;
	};

	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
};

} // namespace util
