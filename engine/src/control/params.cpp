#include "control/params.h"

#include <cstdio>

namespace kp::prm {

#include "control/params_table.inc"

int get(const Record &r, const Def &d)
{
	int v = (r[d.off] >> d.shift) & ((1 << d.bits) - 1);
	if (d.lo < 0 && (v & (1 << (d.bits - 1))))
		v -= 1 << d.bits;
	return v;
}

void put(Record &r, const Def &d, int raw)
{
	const int mask = ((1 << d.bits) - 1) << d.shift;
	r[d.off] = std::uint8_t((r[d.off] & ~mask) | ((raw << d.shift) & mask));
}

std::string text(const Def &d, int raw)
{
	char b[32];
	switch (d.fmt)
	{
	case Fmt::Choice:
	{
		const int k = raw - d.base;
		if (k >= 0 && k < d.nlist)
			return d.list[k];
		std::snprintf(b, sizeof b, "%d", raw);
		return b;
	}
	case Fmt::Signed: std::snprintf(b, sizeof b, raw > 0 ? "+%d" : "%d", raw); return b;
	case Fmt::Note:
	{
		static const char *const n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
		std::snprintf(b, sizeof b, "%s%d", n[((raw % 12) + 12) % 12], raw / 12 - 1);
		return b;
	}
	case Fmt::Oct:
	{
		static const char *const o[4] = { "32'", "16'", "8'", "4'" };
		return raw >= 0 && raw < 4 ? o[raw] : "?";
	}
	case Fmt::Hz: std::snprintf(b, sizeof b, "%+.1fHz", raw / 10.0); return b;
	case Fmt::Slope: std::snprintf(b, sizeof b, "%+.2f", raw * 0.02); return b;
	case Fmt::Pct: std::snprintf(b, sizeof b, "%d%%", raw); return b;
	case Fmt::Db: std::snprintf(b, sizeof b, "%+ddB", raw); return b;
	case Fmt::Cent: std::snprintf(b, sizeof b, "%+dc", raw); return b;
	case Fmt::Num:
	default: std::snprintf(b, sizeof b, "%d", raw); return b;
	}
}

int model_of(int osc_set, int slot)
{
	static const std::uint8_t kSets[12][2] = {
		{ kStd, kStd }, { kStd, kComb }, { kStd, kVpm }, { kStd, kMod }, { kComb, kComb }, { kComb, kVpm },
		{ kComb, kMod }, { kVpm, kVpm }, { kVpm, kMod }, { kBrass, kNone }, { kReed, kNone }, { kPluck, kNone } };
	if (osc_set < 0 || osc_set >= 12 || slot < 1 || slot > 2)
		return kNone;
	return kSets[osc_set][slot - 1];
}

bool active(const Record &r, const Def &d)
{
	return d.model == kNone || model_of(r[kOscSetOffset], d.slot) == d.model;
}

int index_of(int id)
{
	for (int i = 0; i < kCount; i++)
		if (kDefs[i].id == id)
			return i;
	return -1;
}

} // namespace kp::prm
