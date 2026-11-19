// Prophecy program parameters: the edit buffer (the H8's 435-byte program record) and how each
// parameter lives in it. Table: params_table.inc (tools/gen_params.py, from the public Parameter
// Guide; offsets/bitfields checked against the H8 firmware's own parameter table).
//
// How an edit reaches the sound engine (measured on the oracle, reference/wsl/param_sweep.py):
//   - the V55 forwards it to the H8 as the 4-byte message `40 hh ll vv`
//     (hh = ExID << 4 | id >> 8, ll = id & 0xff, vv = raw value byte);
//   - oscillator-model parameters only for the model active in that slot (else dropped);
//   - OSC Set re-sends the whole program record, with each slot whose model changed reset to the
//     V55's default settings for that model (table in the V55 ROM, read from the user's ic12).
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace kp::prm {

enum class Fmt : std::uint8_t { Num, Signed, Choice, Note, Oct, Hz, Slope, Pct, Db, Cent };

struct Def
{
	std::uint16_t id;          // as sent: ExID << 12 | parameter id
	const char *key;           // stable machine name ("p271")
	const char *name;          // short display name (unique)
	const char *section;       // page / group
	std::int16_t lo, hi;       // raw value range
	std::uint16_t off;         // record byte
	std::uint8_t shift, bits;  // bitfield in that byte
	std::uint8_t model;        // 0 = always active; else the oscillator model it belongs to
	std::uint8_t slot;         // oscillator slot (1, 2) for model parameters
	Fmt fmt;
	const char *const *list;   // choice names (Fmt::Choice): raw = base + index
	std::uint8_t nlist;
	std::int16_t base;
};

constexpr int kCount = 525;   // entries in kDefs (params_table.inc asserts it)
extern const Def kDefs[kCount];
extern const std::uint16_t kPeAssign[256];   // Performance Editor number -> parameter id (0 = none)

constexpr int kRecordSize = 435;
constexpr int kOscSetOffset = 138;
constexpr int kOscSetId = 154;
constexpr int kModelBytes = 34;
constexpr int kSettingOffset[3] = { 0, 172, 220 };   // per slot
enum Model { kNone = 0, kStd, kComb, kVpm, kMod, kBrass, kReed, kPluck, kModels };

using Record = std::array<std::uint8_t, kRecordSize>;

int get(const Record &r, const Def &d);        // raw value (sign-extended when the range is signed)
void put(Record &r, const Def &d, int raw);
std::string text(const Def &d, int raw);       // value as the Prophecy would show it
int model_of(int osc_set, int slot);           // Model for slot 1/2 of OSC Set 0..11 (kNone: no osc)
bool active(const Record &r, const Def &d);    // false for a model parameter of an inactive model
int index_of(int id);                          // kDefs index for an id (ExID << 12 | id), -1 if none

} // namespace kp::prm
