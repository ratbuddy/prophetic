// Runtime for H8/300H code translated to C++ by tools/h8xlat.py (control firmware port).
// Holds the register file, condition codes and the Prophecy H8 memory map (1 MiB mode):
//   0x00000-0x3FFFF ROM (user's ic22 via RomView)     0x40000-0x7FFFF SRAM 128 KiB (mirrored)
//   0xC0000-0xC0007 DSP host port (hooked)             0xFFD10-0xFFF0F on-chip RAM
//   0xFFF10-0xFFFFF I/O registers (array + hooks)
// Ours; contains no firmware code or data.
#pragma once

#include "control/rom_view.h"

#include <cstdint>
#include <vector>

namespace kp::h8 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

struct Cpu;

class Hooks
{
public:
	virtual ~Hooks() = default;
	// Native replacement for a call target. Return true if handled (the runtime then pops the
	// return address the caller pushed).
	virtual bool call(u32 /*addr*/, Cpu &) { return false; }
	virtual u8 io_read8(u32 /*addr*/, u8 current) { return current; }
	virtual void io_write8(u32 /*addr*/, u8 /*value*/) {}
	virtual void unsupported(u32 /*pc*/, const char * /*what*/) {}
	// One iteration of a timed busy-wait loop (Cpu::delay_pc): the native layer accounts its time.
	virtual void delay_iteration() {}
};

struct Mem
{
	const kp::ctl::RomView *rom = nullptr;
	Hooks *hooks = nullptr;
	std::vector<u8> sram = std::vector<u8>(0x20000, 0);
	std::vector<u8> onchip = std::vector<u8>(0x200, 0);
	std::vector<u8> io = std::vector<u8>(0xf0, 0);

	u8 rd8(u32 a)
	{
		a &= 0xfffff;
		if (a < 0x40000) return rom ? rom->r8(a) : 0xff;
		if (a < 0x80000) return sram[a & 0x1ffff];
		if (a >= 0xffd10 && a < 0xfff10) return onchip[a - 0xffd10];
		if (a >= 0xfff10) return hooks ? hooks->io_read8(a, io[a - 0xfff10]) : io[a - 0xfff10];
		if (a >= 0xc0000 && a < 0xc0008) return hooks ? hooks->io_read8(a, 0xff) : 0xff;
		return 0xff;
	}
	void wr8(u32 a, u8 v)
	{
		a &= 0xfffff;
		if (a >= 0x40000 && a < 0x80000) sram[a & 0x1ffff] = v;
		else if (a >= 0xffd10 && a < 0xfff10) onchip[a - 0xffd10] = v;
		else if (a >= 0xfff10) { io[a - 0xfff10] = v; if (hooks) hooks->io_write8(a, v); }
		else if (a >= 0xc0000 && a < 0xc0008) { if (hooks) hooks->io_write8(a, v); }
	}
	// Word and longword accesses ignore address bit 0 (H8/300H).
	u16 rd16(u32 a) { a &= ~1u; return u16((rd8(a) << 8) | rd8(a + 1)); }
	u32 rd32(u32 a) { a &= ~1u; return (u32(rd16(a)) << 16) | rd16(a + 2); }
	void wr16(u32 a, u16 v) { a &= ~1u; wr8(a, u8(v >> 8)); wr8(a + 1, u8(v)); }
	void wr32(u32 a, u32 v) { a &= ~1u; wr16(a, u16(v >> 16)); wr16(a + 2, u16(v)); }
};

struct Cpu
{
	u32 er[8] = {};
	bool N = false, Z = false, V = false, C = false;
	u8 ccr_hi = 0x80;   // I, UI, H, U
	Mem *m = nullptr;
	Hooks *hooks = nullptr;
	u64 calls = 0;
	bool unwind = false;   // set by a hook to unwind the translated call chain (task exit/wait)
	u32 spins = 0;         // backward-branch count since the last reset (task/ISR entry)
	u32 spin_limit = 2000000;
	u32 delay_pc = 0;      // backward branch of a timed busy-wait loop (0 = none): Hooks::delay_iteration
	// Called on every backward branch. Past the limit (a poll loop waiting on hardware that the
	// native layer does not model) report the PC once and unwind.
	bool spin(u32 pc)
	{
		if (pc == delay_pc && hooks)
			hooks->delay_iteration();
		if (++spins < spin_limit)
			return false;
		if (spins == spin_limit && hooks)
			hooks->unsupported(pc, "spin limit (hardware poll loop?)");
		unwind = true;
		return true;
	}

	// Register views. 8-bit index: 0-7 = r0h-r7h, 8-15 = r0l-r7l. 16-bit: 0-7 = r0-r7, 8-15 = e0-e7.
	u8 r8(int n) const { return n < 8 ? u8(er[n] >> 8) : u8(er[n - 8]); }
	void w8(int n, u8 v) { if (n < 8) er[n] = (er[n] & 0xffff00ffu) | (u32(v) << 8); else er[n - 8] = (er[n - 8] & 0xffffff00u) | v; }
	u16 r16(int n) const { return n < 8 ? u16(er[n]) : u16(er[n - 8] >> 16); }
	void w16(int n, u16 v) { if (n < 8) er[n] = (er[n] & 0xffff0000u) | v; else er[n - 8] = (er[n - 8] & 0x0000ffffu) | (u32(v) << 16); }

	u8 ccr() const { return u8(ccr_hi | (N << 3) | (Z << 2) | (V << 1) | (C ? 1 : 0)); }
	void set_ccr(u8 v) { ccr_hi = v & 0xf0; N = (v >> 3) & 1; Z = (v >> 2) & 1; V = (v >> 1) & 1; C = v & 1; }

	static u32 msk(int bits) { return bits == 32 ? 0xffffffffu : ((1u << bits) - 1); }
	void nz(u32 r, int bits) { N = (r >> (bits - 1)) & 1; Z = (r & msk(bits)) == 0; }
	u32 mov(u32 r, int bits) { r &= msk(bits); nz(r, bits); V = false; return r; }
	u32 add(u32 a, u32 b, int bits)
	{
		const u32 M = msk(bits); a &= M; b &= M;
		const u32 r = (a + b) & M;
		C = ((u64(a) + b) >> bits) & 1;
		V = ((~(a ^ b) & (a ^ r)) >> (bits - 1)) & 1;
		nz(r, bits);
		return r;
	}
	u32 sub(u32 a, u32 b, int bits)   // a - b
	{
		const u32 M = msk(bits); a &= M; b &= M;
		const u32 r = (a - b) & M;
		C = b > a;
		V = (((a ^ b) & (a ^ r)) >> (bits - 1)) & 1;
		nz(r, bits);
		return r;
	}
	u32 inc(u32 a, u32 n, int bits) { const bool c = C; const u32 r = add(a, n, bits); C = c; return r; }
	u32 dec(u32 a, u32 n, int bits) { const bool c = C; const u32 r = sub(a, n, bits); C = c; return r; }
	u32 logic(u32 r, int bits) { return mov(r, bits); }
	u32 shll(u32 a, int bits) { C = (a >> (bits - 1)) & 1; return logic(a << 1, bits); }
	u32 shal(u32 a, int bits) { C = (a >> (bits - 1)) & 1; const bool v = ((a >> (bits - 1)) ^ (a >> (bits - 2))) & 1; const u32 r = logic(a << 1, bits); V = v; return r; }
	u32 shlr(u32 a, int bits) { a &= msk(bits); C = a & 1; return logic(a >> 1, bits); }
	u32 shar(u32 a, int bits) { a &= msk(bits); C = a & 1; return logic((a >> 1) | (a & (1u << (bits - 1))), bits); }
	u32 rotl(u32 a, int bits) { a &= msk(bits); C = (a >> (bits - 1)) & 1; return logic((a << 1) | (C ? 1 : 0), bits); }
	u32 rotr(u32 a, int bits) { a &= msk(bits); C = a & 1; return logic((a >> 1) | (u32(C) << (bits - 1)), bits); }
	u32 rotxl(u32 a, int bits) { a &= msk(bits); const bool c = C; C = (a >> (bits - 1)) & 1; return logic((a << 1) | (c ? 1 : 0), bits); }
	u32 rotxr(u32 a, int bits) { a &= msk(bits); const bool c = C; C = a & 1; return logic((a >> 1) | (u32(c) << (bits - 1)), bits); }
	u32 exts(u32 a, int from, int to) { const u32 s = (a >> (from - 1)) & 1; u32 r = a & msk(from); if (s) r |= msk(to) & ~msk(from); return mov(r, to); }

	void push32(u32 v) { er[7] -= 4; m->wr32(er[7], v); }
	u32 pop32() { const u32 v = m->rd32(er[7]); er[7] += 4; return v; }
};

// Entry points generated by tools/h8xlat.py.
using Fn = void (*)(Cpu &);
Fn translated(u32 addr);                 // nullptr if not translated
void call(Cpu &c, u32 addr, u32 ret);    // push ret, hook or translated call; logs unknown targets
void tail(Cpu &c, u32 addr);             // jump to another function (its rts returns to our caller)

} // namespace kp::h8
