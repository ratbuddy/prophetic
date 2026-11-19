// Performance Editor: the Prophecy's five PE knobs and four PE sets, computed the way the V55 does
// (reimplemented from its behaviour; every value checked against the oracle with
// reference/wsl/probe_pe.py and engine/test/pe_test.cpp).
//
// A program stores, per PE set (1..4) and knob (1..5), a PE parameter number (Parameter Guide 6.4,
// kPeAssign), the parameter's value at knob 0 and at knob 127 as a percentage of its range, and a
// curve (0 linear, 1 exponential, 2 logarithmic; the two non-linear curves are a 128-entry table in
// the V55 ROM, read from the user's ic12). Moving knob k to x:
//   1. each set's two end values: lo + pct * (hi - lo + 1) / 100, at most hi (C integer division);
//   2. each switched-on set's value at x (interp() below; an earlier set assigned to the same
//      parameter as a later one is dropped, the later one wins);
//   3. a value is applied only if the PE number is valid for the program's OSC Set and
//      Delay/Reverb selection (valid() below) and differs from the parameter's current value.
// Nothing happens unless the knob position changes (the stored program values need not equal the
// knob-0 values: A00 stores +47 where knob 0 computes +46). Turning a PE set on or off changes
// nothing by itself; it applies from the next knob move.
#pragma once

#include "control/params.h"

#include <array>
#include <cstdint>

namespace kp::pe {

constexpr int kKnobs = 5, kSets = 4;
constexpr int kDumpOffset = 444;   // Performance Editor block in the 535-byte program dump
constexpr int kDumpSize = 535;

struct Assign
{
	std::uint8_t pe = 0;      // PE parameter number (0 = none)
	std::uint8_t left = 0;    // % of the range at knob 0
	std::uint8_t right = 0;   // % of the range at knob 127
	std::uint8_t curve = 0;   // 0 linear, 1 exp, 2 log
};

struct Program
{
	Assign a[kSets][kKnobs]{};
	std::uint8_t sw = 0;               // bit s: PE set s+1 on
	std::uint8_t knob[kKnobs]{};       // stored knob positions 0..127
	bool valid = false;

	static Program from_dump(const std::uint8_t *dump);   // 535-byte program dump
};

using Curve = std::array<std::int8_t, 128>;   // V55 curve table (curve 2 = t[x], curve 1 = 127 - t[127 - x])

struct Edit
{
	int index;   // prm::kDefs index
	int value;   // raw value
};

int curve(int x, int c, const Curve &t);
int interp(int a, int b, int x);                    // value at knob x between a (knob 0) and b (knob 127)
bool valid(int pe, const prm::Record &r);           // PE number usable with this record's settings
// The edits knob k at position x makes (sw: switched-on sets), at most kSets; returns the count.
int knob(const Program &p, std::uint8_t sw, int k, int x, const Curve &t, const prm::Record &r, Edit out[kSets]);

} // namespace kp::pe
