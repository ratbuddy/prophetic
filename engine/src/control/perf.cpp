#include "control/perf.h"

namespace kp::pe {

Program Program::from_dump(const std::uint8_t *dump)
{
	Program p;
	for (int s = 0; s < kSets; s++)
		for (int k = 0; k < kKnobs; k++)
		{
			const std::uint8_t *a = dump + kDumpOffset + 20 * s + 4 * k;
			p.a[s][k] = { a[0], a[1], a[2], a[3] };
		}
	p.sw = dump[524] & 0x0f;
	for (int k = 0; k < kKnobs; k++)
		p.knob[k] = dump[525 + k] & 0x7f;
	p.valid = true;
	return p;
}

int curve(int x, int c, const Curve &t)
{
	if (c == 1)
		return 127 - t[127 - x];
	if (c == 2)
		return t[x];
	return x;   // linear (the factory programs use only curves 0..2)
}

// Integer steps as the V55 computes them (32-bit, C division). Narrow ranges map 128 knob steps
// linearly; an odd span wider than 128 is split at its midpoint (knob 64), each half mapped over
// its own knob range. Knob 0 and 127 always give the end values.
int interp(int a, int b, int x)
{
	int lo = a, hi = b;
	if (a > b)
	{
		lo = b;
		hi = a;
		x = 127 - x;
	}
	int res = lo;
	const long span = long(hi) - lo + 1;
	if (span > 128 && span % 2 != 0)
	{
		int si = (hi + lo) / 2;
		if (x >= 65)
		{
			lo = si + 1;
			long s = long(hi) - lo + 1;
			if (s != 0) s = 62000L / s;
			if (s != 0) s = long(x - 65) * 1000L / s;
			si = int(s + lo);
			res = si;
			if (si > lo && si > hi) res = hi;
			if (si < lo && si < hi) res = lo;
		}
		if (x <= 63)
		{
			hi = si - 1;
			long s = long(hi) - lo + 1;
			if (s != 0) s = 63000L / s;
			if (s != 0) s = long(x) * 1000L / s;
			si = int(s + lo);
			res = si;
			if (si > lo && si > hi) res = hi;
			if (si < lo && si < hi) res = lo;
		}
		if (x == 64)
		{
			res = si;
			if (si > lo && si > hi) res = hi;
			if (si < lo && si < hi) res = lo;
		}
	}
	else
	{
		long q = 12800L / span;
		if (q != 0) q = 100L * x / q;
		const int si = int(q + lo);
		res = si;
		if (si > lo && si > hi) res = hi;
		if (si < lo && si < hi) res = lo;
	}
	if (x == 127) res = hi;
	if (x == 0) res = lo;
	return res;
}

static bool in(int v, int a, int b) { return v >= a && v <= b; }

bool valid(int pe, const prm::Record &r)
{
	bool ok = pe != 0;
	// Delay/Reverb selection: chorus/delay parameters (#62..68) or reverb parameters (#69..71)
	if (r[401] == 0 ? in(pe, 0x45, 0x47) : in(pe, 0x3e, 0x44))
		ok = false;
	// The V55 reads the Modulation type here through its panel's edit context, which selects OSC1
	// at power-on and after program changes, so it gets OSC1's first model byte (record 172), not
	// OSC2's Modulation type (220). Reproduced as the oracle behaves (factory program 96, PE #241).
	const int m = r[prm::kSettingOffset[1]];
	auto mod = [&](int from) {
		if (m == 0 && (in(pe, from, 0xee) || pe >= 0xf0)) ok = false;
		if (m == 1 && (in(pe, from, 0xef) || pe >= 0xf2)) ok = false;
		if (m == 2 && (in(pe, from, 0xf1) || pe >= 0xf3)) ok = false;
	};
	// OSC Set: the PE numbers of the oscillator models not in use
	switch (r[prm::kOscSetOffset])
	{
	case 0: if (pe >= 0xc9) ok = false; break;
	case 1: if (in(pe, 0xc3, 0xce) || pe >= 0xd5) ok = false; break;
	case 2: if (in(pe, 0xc3, 0xe1) || pe >= 0xef) ok = false; break;
	case 3: mod(0xc3); break;
	case 4: if (in(pe, 0xbd, 0xc8) || pe >= 0xd5) ok = false; break;
	case 5: if (in(pe, 0xbd, 0xc8) || in(pe, 0xcf, 0xe1) || pe >= 0xef) ok = false; break;
	case 6: if (in(pe, 0xbd, 0xc8)) ok = false; mod(0xcf); break;
	case 7: if (in(pe, 0xbd, 0xd4) || pe >= 0xef) ok = false; break;
	case 8: if (in(pe, 0xbd, 0xd4)) ok = false; mod(0xe2); break;
	case 9: if (in(pe, 0xbd, 0xf2) || pe >= 0xf8) ok = false; break;
	case 10: if (in(pe, 0xbd, 0xf7) || pe >= 0xfb) ok = false; break;
	case 11: if (in(pe, 0xbd, 0xfa)) ok = false; break;
	default: break;
	}
	return ok;
}

int knob(const Program &p, std::uint8_t sw, int k, int x, const Curve &t, const prm::Record &r, Edit out[kSets])
{
	if (!p.valid || k < 0 || k >= kKnobs)
		return 0;
	x = x < 0 ? 0 : (x > 127 ? 127 : x);
	int id[kSets], idx[kSets], lv[kSets] = {}, rv[kSets] = {}, cur[kSets] = {};
	bool on[kSets];
	for (int s = 0; s < kSets; s++)
	{
		const Assign &a = p.a[s][k];
		id[s] = prm::kPeAssign[a.pe];
		idx[s] = id[s] ? prm::index_of(id[s]) : -1;
		if (idx[s] < 0)
			id[s] = 0;
		else
		{
			const prm::Def &d = prm::kDefs[idx[s]];
			const int span = d.hi - d.lo + 1;
			lv[s] = d.lo + a.left * span / 100;
			rv[s] = d.lo + a.right * span / 100;
			if (lv[s] > d.hi) lv[s] = d.hi;
			if (rv[s] > d.hi) rv[s] = d.hi;
		}
		on[s] = (sw >> s) & 1;
	}
	for (int s = 0; s < kSets; s++)
	{
		if (!on[s])
		{
			id[s] = 0;
			continue;
		}
		cur[s] = interp(lv[s], rv[s], curve(x, p.a[s][k].curve, t));
		for (int j = 0; j < s; j++)   // the same parameter in an earlier set: the later set wins
			if (id[j] == id[s] && on[j])
				id[j] = 0;
	}
	int n = 0;
	for (int s = 0; s < kSets; s++)
		if (id[s] > 0 && valid(p.a[s][k].pe, r) && prm::get(r, prm::kDefs[idx[s]]) != cur[s])
			out[n++] = { idx[s], cur[s] };
	return n;
}

} // namespace kp::pe
