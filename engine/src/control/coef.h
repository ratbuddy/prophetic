// Parameter-to-coefficient compute functions (port of the H8 control firmware's per-slot
// transforms; see docs/H8.md "Parameter to coefficient layer"). Each takes the 32-bit input the
// firmware passes in er0 and returns the 32-bit coefficient written to the DSP.
// Firmware-version specifics (which slot uses which transform, ROM tables) come from the user's
// ROM at run time; nothing firmware-derived is hard-coded here.
#pragma once

#include "control/rom_view.h"

#include <cstdint>

namespace kp::ctl {

// Full per-slot transform by firmware entry address (v2.0 addresses; table data from `rom`).
// `known` is false for an unported entry (input returned unchanged).
std::uint32_t coef_compute(std::uint32_t fn, std::uint32_t in, const RomView &rom, bool &known);

// Identity: coefficient = input (80 slots on v2.0).
inline std::uint32_t coef_identity(std::uint32_t in) { return in; }

// Signed 16-bit -> signed 32-bit full-scale expansion (72 slots on v2.0).
// -0x8000 is treated as -0x7fff; |x| = 0x7fff maps to 0x7fffffff; otherwise |x| maps to
// (|x| << 16) | ((2|x|) & 0xffff); the sign is re-applied with a 32-bit negate.
inline std::uint32_t coef_s16_full(std::uint32_t in)
{
	std::uint16_t x = std::uint16_t(in);
	if (x == 0x8000)
		x = 0x8001;
	const bool neg = (x & 0x8000) != 0;
	const std::uint16_t mag = neg ? std::uint16_t(-x) : x;
	std::uint32_t v;
	if (mag == 0x7fff)
		v = 0x7fffffffu;
	else
		v = (std::uint32_t(mag) << 16) | std::uint16_t(mag + mag);
	return neg ? std::uint32_t(-std::int64_t(v)) : v;
}

// Unsigned 16-bit centred at 0x7fff -> signed 32-bit full scale (2 slots on v2.0).
inline std::uint32_t coef_u16_centred(std::uint32_t in)
{
	return coef_s16_full((in & 0xffff0000u) | std::uint16_t(std::uint16_t(in) - 0x7fff));
}

} // namespace kp::ctl
