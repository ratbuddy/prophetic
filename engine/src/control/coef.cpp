// Port of the H8 per-slot coefficient transforms (v2.0 / korgpro20 entry addresses).
// Each case is a line-by-line translation of the firmware routine on a two-register model
// (er0, er1) with the H8/300H operand-size rules: 16-bit ops touch only Rn, mulxu/mulxs.w use the
// low word of the destination, divxu.w leaves the remainder in En. ROM tables are read from the
// user's ROM through RomView; no firmware data lives in this file.
#include "control/coef.h"

namespace kp::ctl {

namespace {

struct R
{
	std::uint32_t er0 = 0, er1 = 0;
	std::uint16_t r0() const { return std::uint16_t(er0); }
	std::uint16_t e0() const { return std::uint16_t(er0 >> 16); }
	std::uint16_t r1() const { return std::uint16_t(er1); }
	std::uint16_t e1() const { return std::uint16_t(er1 >> 16); }
	void set_r0(std::uint16_t v) { er0 = (er0 & 0xffff0000u) | v; }
	void set_e0(std::uint16_t v) { er0 = (er0 & 0x0000ffffu) | (std::uint32_t(v) << 16); }
	void set_r1(std::uint16_t v) { er1 = (er1 & 0xffff0000u) | v; }
	void add_b_r0l(std::uint8_t v) { er0 = (er0 & ~0xffu) | std::uint8_t(std::uint8_t(er0) + v); }
	void extu_w0() { set_r0(std::uint8_t(er0)); }
	void extu_l0() { er0 = r0(); }
	void shll_w0() { set_r0(std::uint16_t(r0() << 1)); }
};

std::uint32_t mulxu(std::uint16_t a, std::uint16_t b) { return std::uint32_t(a) * b; }
std::uint32_t mulxs(std::uint16_t a, std::uint16_t b) { return std::uint32_t(std::int32_t(std::int16_t(a)) * std::int16_t(b)); }
std::uint32_t shar(std::uint32_t v, int n) { return std::uint32_t(std::int32_t(v) >> n); }
std::uint32_t neg(std::uint32_t v) { return std::uint32_t(0) - v; }
// divxu.w rs, erd: quotient -> Rd, remainder -> Ed (callers here immediately extu.l)
std::uint32_t divxu_q(std::uint32_t erd, std::uint16_t rs) { return rs ? std::uint16_t(erd / rs) : 0; }

// add.b #h'63, r0l ; extu.w r0 ; shll.w r0 (x shifts) ; extu.l er0  -> byte index into a table
std::uint32_t idx63(std::uint32_t in, int shifts)
{
	R s; s.er0 = in;
	s.add_b_r0l(0x63);
	s.extu_w0();
	for (int i = 0; i < shifts; i++) s.shll_w0();
	s.extu_l0();
	return s.er0;
}

std::uint32_t t1044(std::uint32_t in, const RomView &rom) { return rom.r32(0x001044 + idx63(in, 2)); }
std::uint16_t tcb6(std::uint32_t in, const RomView &rom) { return rom.r16(0x000cb6 + idx63(in, 1)); }

// shll.w r0 x2 ; extu.l er0 ; mov.l @(table, er0), er0
std::uint32_t tw(std::uint32_t in, std::uint32_t table, const RomView &rom) { return rom.r32(table + std::uint16_t(std::uint16_t(in) << 2)); }
// sub.w e0, e0 ; shll.l er0 x2 ; mov.l @(table, er0), er0
std::uint32_t tl(std::uint32_t in, std::uint32_t table, const RomView &rom) { return rom.r32(table + (std::uint32_t(std::uint16_t(in)) << 2)); }
// add.b r0l, r0l x2 ; extu.w r0 ; extu.l er0 ; mov.l @(table, er0), er0
std::uint32_t tb(std::uint32_t in, std::uint32_t table, const RomView &rom) { return rom.r32(table + std::uint8_t(std::uint8_t(in) << 2)); }

} // namespace

std::uint32_t coef_compute(std::uint32_t fn, std::uint32_t in, const RomView &rom, bool &known)
{
	known = true;
	R s; s.er0 = in;
	switch (fn)
	{
	case 0x017cf2: return coef_identity(in);
	case 0x017e0a: return coef_s16_full(in);
	case 0x0183d2: return coef_u16_centred(in);
	case 0x017d3e: return t1044(in, rom);
	case 0x018154: return neg(t1044(in, rom));
	case 0x018184: return neg(t1044(in, rom));
	case 0x01818c: return shar(t1044(in, rom), 2);
	case 0x0181b4: return t1044(in, rom) - 0x7fffffffu;
	case 0x0183a0: return t1044(in, rom) >> 1;
	case 0x0183b8: return t1044(in, rom) >> 2;
	case 0x018422: return shar(t1044(in, rom), 1);
	case 0x017f62: return shar(neg(t1044(in, rom) - 0x7fffffffu), 1);
	case 0x0181d0: return coef_s16_full(in) >> 1;

	case 0x017f32: return tw(in, 0x017f26, rom);
	case 0x017f50: return tw(in, 0x017f44, rom);
	case 0x018240: return tw(in, 0x018230, rom);
	case 0x0182d2: return tw(in, 0x0182c6, rom);
	case 0x0182f0: return tw(in, 0x0182e4, rom);
	case 0x01830e: return tw(in, 0x018302, rom);
	case 0x01832c: return tw(in, 0x018320, rom);
	case 0x018352: return tw(in, 0x01833e, rom);
	case 0x018378: return tw(in, 0x018364, rom);
	case 0x0183e4: return tw(in, 0x0183dc, rom);
	case 0x0183fe: return tw(in, 0x0183f6, rom);

	case 0x01802e: return tl(in, 0x018022, rom);
	case 0x018050: return tl(in, 0x018040, rom);
	case 0x01806a: return tl(in, 0x018062, rom);
	case 0x018088: return tl(in, 0x01807c, rom);
	case 0x0180a6: return tl(in, 0x01809a, rom);
	case 0x0180c4: return tl(in, 0x0180b8, rom);
	case 0x0180e6: return tl(in, 0x0180d6, rom);
	case 0x0181a2: return tl(in, 0x018196, rom);

	case 0x017f86: return tb(in, 0x017f72, rom);
	case 0x017fae: return tb(in, 0x017f9a, rom);
	case 0x017fd6: return tb(in, 0x017fc2, rom);
	case 0x017ffe: return tb(in, 0x017fea, rom);

	case 0x018012:   // mulxu.w #h'76ff ; shal.l ; add.l #h'09000000
		return (mulxu(s.r0(), 0x76ff) << 1) + 0x09000000u;
	case 0x018200:   // e0 = h'3fe0 ; mulxu.w e0, er0 ; add.l #h'00100000
		return mulxu(s.r0(), 0x3fe0) + 0x00100000u;
	case 0x018222:   // e0 = h'fffe ; mulxu.w e0, er0
		return mulxu(s.r0(), 0xfffe);
	case 0x01820e:   // er0 = r0*r0 ; shll.l ; r0 = h'dff0 ; mulxs.w e0, er0 ; add.l #h'fff80000
		s.er0 = mulxu(s.r0(), s.r0()) << 1;
		s.set_r0(0xdff0);
		return mulxs(s.r0(), s.e0()) + 0xfff80000u;
	case 0x018410:   // r1 = h'077f ; mulxu.w r0, er1 ; shll.l er1 ; er0 = h'00800000 + er1
		return 0x00800000u + (mulxu(0x077f, s.r0()) << 1);
	case 0x01847e:   // mov.w r0, e0 ; shar.l x3
		return shar((std::uint32_t(s.r0()) << 16) | s.r0(), 3);
	case 0x0184ac:   // e0 = h'debf ; mulxu.w e0, er0 ; divxu.w #99 ; extu.l ; add.l #h'00003240
		return divxu_q(mulxu(s.r0(), 0xdebf), 0x63) + 0x00003240u;
	case 0x0184c2:
		return divxu_q(mulxu(s.r0(), 0xdebf), 0x63) + 0x00011340u;
	case 0x0184f0:   // inc.w r0 ; e0 = h'2580 ; mulxu.w e0, er0 ; divxu.w #100 ; extu.l
		return divxu_q(mulxu(std::uint16_t(s.r0() + 1), 0x2580), 0x64);

	case 0x0180f8:   // r0 = tcb6 ; e0 = h'7c00 ; mulxs.w e0, er0 ; shal.l ; add.l #h'80000001
		return (mulxs(tcb6(in, rom), 0x7c00) << 1) + 0x80000001u;
	case 0x01811a:   // r0 = tcb6 ; e0 = h'0520 ; mulxu.w e0, er0
	case 0x01816c:
		return mulxu(tcb6(in, rom), 0x0520);
	case 0x018132:   // r0 = tcb6 ; er0 = r0*r0 ; shll.l ; r0 = h'0ccc ; mulxs.w e0, er0 ; shal.l ; neg.l
	{
		const std::uint16_t t = tcb6(in, rom);
		s.er0 = mulxu(t, t) << 1;
		s.set_r0(0x0ccc);
		return neg(mulxs(s.r0(), s.e0()) << 1);
	}
	case 0x0181d8:   // r0 = tcb6 ; er1 = r0*r0 << 1 ; er0 = r0*e1 << 1 ; r0 = h'be00 ; er0 = r0*e0 ; + h'01000000
	{
		const std::uint16_t t = tcb6(in, rom);
		s.er0 = t;
		s.er1 = mulxu(t, t) << 1;
		s.er0 = mulxu(s.r0(), s.e1()) << 1;
		s.set_r0(0xbe00);
		return mulxu(s.r0(), s.e0()) + 0x01000000u;
	}
	case 0x01843a:   // r1 = h'0a00 ; er1 = r1*tcb6 << 1 ; er0 = h'7a000000 - er1
		return 0x7a000000u - (mulxu(0x0a00, tcb6(in, rom)) << 1);
	case 0x01845c:
		return 0x7fe00000u - (mulxu(0x05e0, tcb6(in, rom)) << 1);
	case 0x018488:   // er0 = tcb6 * h'0129 ; shlr.l x3 ; add.l #h'00003000
		return (mulxu(tcb6(in, rom), 0x0129) >> 3) + 0x00003000u;
	case 0x0184d8:   // r0 = -tcb6 ; e0 = r0 ; r0 = 0
		return std::uint32_t(std::uint16_t(-tcb6(in, rom))) << 16;
	}
	known = false;
	return in;
}

} // namespace kp::ctl
