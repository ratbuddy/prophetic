#include "synth.h"
#include "control/sysex.h"
#include "dsp_fast.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace kp {

namespace {

std::vector<std::uint8_t> slurp(const std::filesystem::path &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::uint32_t crc32(const std::vector<std::uint8_t> &d)
{
	std::uint32_t c = 0xffffffffu;
	for (std::uint8_t b : d)
	{
		c ^= b;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

constexpr int kFramesPerUartByte = 16;   // 31.25 kbaud, 10 bits/byte = 3125 B/s ~ one byte per 15.4 frames

// V55 -> H8 message lengths by type (high nibble); type 5 = 16-bit length + data.
int message_length(const std::uint8_t *p, std::size_t avail)
{
	switch (p[0] >> 4)
	{
	case 0x0: case 0x1: return 3;
	case 0x3: case 0x4: case 0xB: return 4;
	case 0x8: case 0x9: case 0xA: case 0xC: case 0xE: return 2;
	case 0x5: return avail >= 3 ? 3 + ((p[1] << 8) | p[2]) : 1;
	default: return 1;
	}
}

// What the engine reads from the V55 ROM (ic12), per firmware version (CRC32 of ic12):
//   defaults: the oscillator-model default settings, 7 x 34 bytes in model order (Standard, Comb,
//             VPM, Modulation, Brass, Reed, Pluck); located by matching the records the V55 sends
//             on OSC Set changes (reference/wsl/probe_oscsets.py)
//   programs: the 128 factory programs as 535-byte dumps in program order (their Performance
//             Editor settings; each is matched to the captured program by its name)
//   curve:    the Performance Editor's 128-entry curve table (located by tracing the V55's knob
//             handling on the oracle; values checked with reference/wsl/probe_pe.py)
//   patterns: the 10 internal arpeggio patterns, 128 bytes each (Parameter Guide 6.6 layout;
//             played back on the oracle by reference/wsl/arp_expect.py)
struct V55At { std::uint32_t crc, defaults, programs, curve, patterns; };
constexpr V55At kV55[] = { { 0xf33b7932u, 0x3c587, 0x3ff60, 0x77676, 0x3e542 } };   // korgpro20 (v2.0)

// The V55's program-change burst, measured on the oracle (reference/wsl/probe_pe.py; every factory
// program and program-to-program changes, byte for byte):
//   1. controller resets (constant)
//   2. the previous program's Wheel1, Wheel2, Wheel3+/-, Ribbon X / Z controllers (Controller
//      Assignment, dump bytes 435..440) to 0
//   3. the program record (`50 01 b3` + the dump's first 435 bytes)
//   4. the new program's wheel controllers at their panel positions: Wheel1 0, Wheel2 0x4040,
//      Wheel3 0x4040 when both halves share a controller, else each half 0; then Porta SW (CC65)
//   5. every Performance Editor assignment (knob by knob, set by set) as a `40` edit of its
//      parameter's current value
// The same wheel block, CC91..95 and Porta SW follow the record the V55 re-sends on OSC Set changes.
constexpr std::uint8_t kResetControllers[][3] = { { 0x0e, 0x00, 0x00 }, { 0x0f, 0x00, 0x00 }, { 0x11, 0x00, 0x00 },
	{ 0x12, 0x00, 0x00 }, { 0x15, 0x40, 0x00 }, { 0x17, 0x7f, 0xff }, { 0x1a, 0x40, 0x00 }, { 0x1b, 0x7f, 0xff },
	{ 0x50, 0x00, 0x00 }, { 0x6b, 0x40, 0x00 }, { 0x6c, 0x40, 0x00 }, { 0x6d, 0x40, 0x00 }, { 0x6e, 0x40, 0x00 },
	{ 0x6f, 0x40, 0x00 } };
constexpr std::uint8_t kEffectControllers[] = { 0x6b, 0x6c, 0x6e, 0x6d, 0x6f };   // OSC Set reload order
constexpr std::uint8_t kBootAssign[9] = { 1, 6, 7, 8, 21, 22, 23, 1, 0 };   // A00's (the boot program)

int assign_id(int code)   // Controller Assignment code -> H8 controller id (-1: Off)
{
	if (code <= 0 || code > 100) return -1;
	if (code <= 3) return 0x0e;   // pitch bend +/-, +, -
	if (code == 4) return 0x0f;   // aftertouch
	return 0x10 + code - 5;       // CC#0..95
}

std::size_t put_ctl(std::uint8_t *o, int id, int v)
{
	o[0] = 0x30; o[1] = std::uint8_t(id); o[2] = std::uint8_t(v >> 8); o[3] = std::uint8_t(v);
	return 4;
}

// the program's wheel controllers at their panel positions (burst part 4, OSC Set reload)
std::size_t wheel_block(const std::uint8_t *assign, std::uint8_t *o)
{
	std::size_t n = 0;
	if (assign_id(assign[0]) >= 0) n += put_ctl(o + n, assign_id(assign[0]), 0);
	if (assign_id(assign[1]) >= 0) n += put_ctl(o + n, assign_id(assign[1]), 0x4040);
	if (assign[2] == assign[3])
	{
		if (assign_id(assign[2]) >= 0) n += put_ctl(o + n, assign_id(assign[2]), 0x4040);
	}
	else
	{
		if (assign_id(assign[2]) >= 0) n += put_ctl(o + n, assign_id(assign[2]), 0);
		if (assign_id(assign[3]) >= 0) n += put_ctl(o + n, assign_id(assign[3]), 0);
	}
	return n;
}

std::string trimmed_name(const std::uint8_t *dump)
{
	std::string s(reinterpret_cast<const char *>(dump), 16);
	for (char &ch : s)   // 0x7F is the LCD's left arrow (HD44780); other non-ASCII bytes: space
		ch = ch == 0x7f ? '<' : (ch < 0x20 || ch > 0x7e ? ' ' : ch);
	while (!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}

} // namespace

Synth::Program Synth::program_from_dump(const std::uint8_t *dump, const std::string &label) const
{
	Program p;
	p.name = label + " " + trimmed_name(dump);
	std::memcpy(p.dump.data(), dump, pe::kDumpSize);
	p.has_dump = true;
	std::memcpy(p.record.data(), dump, prm::kRecordSize);
	p.porta = (dump[530] & 1) != 0;
	p.perf = pe::Program::from_dump(dump);
	return p;
}

std::size_t Synth::build_burst(const Program &p, const std::uint8_t *prev_assign, std::uint8_t *o) const
{
	std::size_t n = 0;
	for (const auto &c : kResetControllers)
		n += put_ctl(o + n, c[0], (c[1] << 8) | c[2]);
	for (int k = 0; k < 6; k++)   // the previous program's wheels and ribbon
		if (assign_id(prev_assign[k]) >= 0)
			n += put_ctl(o + n, assign_id(prev_assign[k]), 0);
	o[n++] = 0x50;
	o[n++] = std::uint8_t(prm::kRecordSize >> 8);
	o[n++] = std::uint8_t(prm::kRecordSize);
	std::memcpy(o + n, p.dump.data(), prm::kRecordSize);
	n += prm::kRecordSize;
	n += wheel_block(&p.dump[435], o + n);
	n += put_ctl(o + n, 0x51, p.porta ? 0x7fff : 0);
	n += pe_edits(p, o + n);
	return n;
}

std::size_t Synth::pe_edits(const Program &p, std::uint8_t *o) const
{
	std::size_t n = 0;
	for (int k = 0; k < pe::kKnobs; k++)   // Performance Editor assignments, as edits of their current values
		for (int s = 0; s < pe::kSets; s++)
		{
			const int pen = p.dump[pe::kDumpOffset + 20 * s + 4 * k];
			const int id = pen ? prm::kPeAssign[pen] : 0;
			const int idx = id ? prm::index_of(id) : -1;
			if (idx < 0)
				continue;
			o[n++] = 0x40;
			o[n++] = std::uint8_t(id >> 8);
			o[n++] = std::uint8_t(id);
			o[n++] = std::uint8_t(prm::get(p.record, prm::kDefs[idx]));
		}
	return n;
}

std::vector<std::uint8_t> Synth::h8_ram() const { return ctl_->ram_image(); }

bool Synth::save_snapshot(const std::string &dir) const
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::create_directories(dir, ec);
	const auto state = board_->save_state();
	const auto ram = ctl_->ram_image();
	std::ofstream a(fs::path(dir) / "state.bin", std::ios::binary);
	a.write(reinterpret_cast<const char *>(state.data()), std::streamsize(state.size()));
	std::ofstream b(fs::path(dir) / "ram.bin", std::ios::binary);
	b.write(reinterpret_cast<const char *>(ram.data()), std::streamsize(ram.size()));
	return bool(a) && bool(b);
}

void Synth::run_frames(std::uint64_t until)
{
	float l[256], r[256];
	while (frame_ < until)
		render(l, r, int(std::min<std::uint64_t>(256, until - frame_)));
}

void Synth::run_until_sent()
{
	float l[16], r[16];
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (tail_ == head_ && byte_gap_ == 0)
				return;
		}
		render(l, r, 1);
	}
}

// Power-on without a snapshot (no state.bin / ram.bin): the H8 runs its reset handler on a fresh
// board, then receives the V55's power-on messages as the oracle's V55 sends them (korgpro20,
// measured byte for byte, with their pauses), and the engine runs on to kBootFrames:
//   1.4747 s after reset: 80 00 90 00 a0 00 c0 01, User Scale 1 and 2 at their defaults (b0 00 nn 00
//     x 12, b0 01 nn 00 x 128);
//   368 frames later the controller resets (as in a program burst), 46 frames later A00's program
//     record;
//   8342 frames later, one message every 82 frames: the panel controllers at their power-on
//     positions in the V55's power-on order (A00's Controller Assignment bytes 0, 4, 1, 2, 3, 6, then
//     aftertouch; wheel 2 at its centre), Porta SW and the Performance Editor edits (as in a burst).
// The boot program is A00 (the V55's power-on program), from the user's ic12.
void Synth::boot_native()
{
	const Program &a00 = programs_[0];
	ctl_->boot(rom_);
	frame_ = 0;
	byte_gap_ = 0;
	auto push = [&](const std::vector<std::uint8_t> &m) {
		std::lock_guard<std::mutex> lk(mu_);
		push_locked(m.data(), m.size());
	};
	std::vector<std::uint8_t> g = { 0x80, 0x00, 0x90, 0x00, 0xa0, 0x00, 0xc0, 0x01 };
	for (int k = 0; k < 12; k++)
		g.insert(g.end(), { 0xb0, 0x00, std::uint8_t(k), 0x00 });
	for (int k = 0; k < 128; k++)
		g.insert(g.end(), { 0xb0, 0x01, std::uint8_t(k), 0x00 });
	run_frames(70783);
	push(g);
	run_until_sent();
	run_frames(frame_ + 368);
	g.assign(14 * 4, 0);
	std::size_t n = 0;
	for (const auto &c : kResetControllers)
		n += put_ctl(g.data() + n, c[0], (c[1] << 8) | c[2]);
	push(g);
	run_until_sent();
	run_frames(frame_ + 46);
	g = { 0x50, std::uint8_t(prm::kRecordSize >> 8), std::uint8_t(prm::kRecordSize) };
	g.insert(g.end(), a00.dump.begin(), a00.dump.begin() + prm::kRecordSize);
	push(g);
	run_until_sent();
	run_frames(frame_ + 8342);
	std::vector<std::vector<std::uint8_t>> msgs;
	const std::uint8_t *a = &a00.dump[435];
	for (int k : { 0, 4, 1, 2, 3, 6 })
		if (assign_id(a[k]) >= 0)
		{
			std::uint8_t m[4];
			put_ctl(m, assign_id(a[k]), k == 1 ? 0x4040 : 0);
			msgs.emplace_back(m, m + 4);
		}
	{
		std::uint8_t m[4];
		put_ctl(m, 0x0f, 0);   // aftertouch
		msgs.emplace_back(m, m + 4);
		put_ctl(m, 0x51, a00.porta ? 0x7fff : 0);
		msgs.emplace_back(m, m + 4);
	}
	std::uint8_t pe[kBurstMax];
	const std::size_t np = pe_edits(a00, pe);
	for (std::size_t k = 0; k + 4 <= np; k += 4)
		msgs.emplace_back(pe + k, pe + k + 4);
	for (const auto &m : msgs)
	{
		const std::uint64_t start = frame_;
		push(m);
		run_frames(start + 82);
	}
	run_until_sent();
	run_frames(kBootFrames);
	frame_ = 0;   // the engine runs on as from a snapshot taken here (an H8 tick is due)
}

std::vector<std::uint8_t> Synth::program_burst(int index, const std::uint8_t *prev_assign) const
{
	if (index < 0 || index >= int(programs_.size()))
		return {};
	const Program &p = programs_[index];
	if (!p.has_dump)
		return p.bytes;
	std::vector<std::uint8_t> b(kBurstMax);
	b.resize(build_burst(p, prev_assign ? prev_assign : kBootAssign, b.data()));
	return b;
}

int Synth::load_user_sysex(const std::string &dir, std::string *report)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return 0;
	std::vector<fs::path> files;
	for (const auto &e : fs::directory_iterator(dir, ec))
	{
		std::string ext = e.path().extension().string();
		for (char &ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (e.is_regular_file(ec) && ext == ".syx")
			files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	int added = 0, patterns = 0;
	for (const auto &f : files)
	{
		const auto bytes = slurp(f);
		const auto c = syx::parse(bytes.data(), bytes.size());
		for (const auto &p : c.programs)
		{
			char label[16];
			std::snprintf(label, sizeof label, "U%03d", added + 1);
			add_program(p.data.data(), label);
			added++;
		}
		for (const auto &p : c.patterns)
			if (p.number >= 0 && p.number < 10)
			{
				std::memcpy(arp_patterns_[p.number].data(), p.data.data(), 128);
				have_arp_patterns_ = true;
				patterns++;
			}
		if (report)
			*report += f.filename().string() + ": " + std::to_string(c.programs.size()) + " programs, " +
			           std::to_string(c.patterns.size()) + " patterns\n";
	}
	return added;
}

bool Synth::program_dump(int index, std::uint8_t *out) const
{
	if (index < 0 || index >= int(programs_.size()) || !programs_[index].has_dump)
		return false;
	std::memcpy(out, programs_[index].dump.data(), pe::kDumpSize);
	return true;
}

int Synth::add_program(const std::uint8_t *dump, const std::string &label)
{
	programs_.push_back(program_from_dump(dump, label));
	return int(programs_.size()) - 1;
}

bool Synth::load(const std::string &dir, std::string &error, bool native_boot)
{
	namespace fs = std::filesystem;
	const fs::path d(dir);
	if (!rom_.load_ic22((d / "ic22.bin").string())) { error = "cannot read ic22.bin"; return false; }
	const bool snapshot = !native_boot && fs::exists(d / "state.bin") && fs::exists(d / "ram.bin");
	dsp_fast_prefault();   // compiled DSP code resident before the audio thread needs it
	board_ = std::make_unique<ProphecyBoard>();
	ctl_ = std::make_unique<ctl::H8Control>(*board_);
	if (snapshot)
	{
		const auto state = slurp(d / "state.bin");
		const auto ram = slurp(d / "ram.bin");
		if (!board_->load_state(state.data(), state.size())) { error = "bad state.bin"; return false; }
		if (!ctl_->load(rom_, ram.data(), ram.size())) { error = "bad ram.bin"; return false; }
	}

	have_defaults_ = have_perf_ = false;
	std::vector<std::uint8_t> v55;
	const V55At *at = nullptr;
	if (fs::exists(d / "ic12.bin"))
	{
		v55 = slurp(d / "ic12.bin");
		const std::uint32_t crc = crc32(v55);
		for (const auto &a : kV55)
			if (a.crc == crc && v55.size() >= a.programs + 128u * pe::kDumpSize && v55.size() >= a.curve + 128u &&
			    v55.size() >= a.patterns + 10u * 128u)
				at = &a;
	}
	if (at)
	{
		for (int m = 1; m < prm::kModels; m++)
			std::memcpy(model_defaults_[m].data(), &v55[at->defaults + (m - 1) * prm::kModelBytes], prm::kModelBytes);
		have_defaults_ = true;
		for (int k = 0; k < 128; k++)
			curve_[k] = std::int8_t(v55[at->curve + k]);
		have_perf_ = true;
		for (int p = 0; p < 10; p++)
			std::memcpy(arp_patterns_[p].data(), &v55[at->patterns + 128u * p], 128);
		have_arp_patterns_ = true;
	}

	programs_.clear();
	std::memcpy(assign_.data(), kBootAssign, sizeof kBootAssign);
	for (int i = 0; i < 128; i++)
	{
		char base[16];
		std::snprintf(base, sizeof base, "%03d", i);
		if (at)   // the factory programs from the user's ic12: bursts built as the V55 builds them
		{
			programs_.push_back(program_from_dump(&v55[at->programs + std::size_t(i) * pe::kDumpSize], base));
			continue;
		}
		const fs::path up = d / "programs" / (std::string(base) + ".uart");   // without ic12: captured bursts
		if (!fs::exists(up))
			continue;
		Program p;
		p.bytes = slurp(up);
		const auto nm = slurp(d / "programs" / (std::string(base) + ".name"));
		p.name = std::string(base) + " " + std::string(nm.begin(), nm.end());
		bool after_record = false;
		for (std::size_t k = 0; k < p.bytes.size();)
		{
			const std::uint8_t *m = &p.bytes[k];
			const int len = message_length(m, p.bytes.size() - k);
			if (k + len > p.bytes.size())
				break;
			if (m[0] == 0x50 && len == 3 + prm::kRecordSize)
			{
				std::memcpy(p.record.data(), m + 3, prm::kRecordSize);
				after_record = true;
			}
			else if (m[0] == 0x40 && after_record)
				p.edits.push_back({ m[0], m[1], m[2], m[3] });
			else if (m[0] == 0x30 && m[1] == 0x51)
				p.porta = ((m[2] << 8) | m[3]) != 0;
			k += len;
		}
		programs_.push_back(std::move(p));
	}
	if (!snapshot)
	{
		if (programs_.empty() || !programs_[0].has_dump)
		{
			error = "no state.bin / ram.bin, and no supported ic12.bin to boot from";
			return false;
		}
		boot_native();
		booted_ = true;
	}
	if (!programs_.empty())
	{
		// The engine is at the end of the boot (A00 loaded): start from that program's edit buffer.
		edit_ = programs_[0].record;
		for (const auto &e : programs_[0].edits)
			apply_edit(edit_, e.data());
		track_controllers(program_burst(0));
		use_program_perf_locked(programs_[0]);
		if (programs_[0].has_dump)
		{
			std::memcpy(assign_.data(), &programs_[0].dump[435], assign_.size());
			cur_dump_ = programs_[0].dump;
		}
	}
	return true;
}

void Synth::use_program_perf_locked(const Program &p)
{
	perf_ = p.perf;
	pe_sw_ = p.perf.valid ? p.perf.sw : 0;
	for (int k = 0; k < pe::kKnobs; k++)
		knob_[k] = p.perf.valid ? p.perf.knob[k] : 0;
}

void Synth::push_locked(const std::uint8_t *p, std::size_t n)
{
	// All or nothing: a partial message would desynchronise the H8's parser. The ring holds 64 KB
	// (about 20 s of UART traffic); only a flood of program changes can fill it.
	const std::size_t used = (head_ - tail_) & (ring_.size() - 1);
	if (used + n >= ring_.size())
		return;
	for (std::size_t i = 0; i < n; i++)
	{
		ring_[head_] = p[i];
		head_ = (head_ + 1) & (ring_.size() - 1);
	}
}

void Synth::wake_locked()
{
	if (!sleeping_.load(std::memory_order_relaxed))
		return;
	for (int k = 0; k < held_count_; k++)   // what was held while asleep goes first, in its order
	{
		const int s = held_order_[size_t(k)];
		push_locked(held_[size_t(s)].data(), held_len_[size_t(s)]);
		held_len_[size_t(s)] = 0;
	}
	held_count_ = 0;
	sleeping_.store(false, std::memory_order_relaxed);   // continue where it stopped
	silent_frames_ = 0;
}

void Synth::post_locked(const std::uint8_t *p, std::size_t n)
{
	wake_locked();
	push_locked(p, n);
}

void Synth::post_value_locked(int slot, const std::uint8_t *p, std::size_t n)
{
	if (!(idle_sleep_ && sleeping_.load(std::memory_order_relaxed)))
	{
		push_locked(p, n);
		return;
	}
	if (held_len_[size_t(slot)] == 0)
		held_order_[size_t(held_count_++)] = std::uint16_t(slot);
	std::memcpy(held_[size_t(slot)].data(), p, n);   // the latest value of this slot
	held_len_[size_t(slot)] = std::uint8_t(n);
}

void Synth::set_idle_sleep(bool on)
{
	if (on == idle_sleep_)
		return;
	std::lock_guard<std::mutex> lk(mu_);
	if (!on)
		wake_locked();
	idle_sleep_ = on;
}

void Synth::send(std::initializer_list<std::uint8_t> bytes)
{
	std::lock_guard<std::mutex> lk(mu_);
	const std::uint8_t *m = bytes.begin();
	int slot = -1;
	switch (m[0])
	{
	case 0x80: slot = kSlotGlobal; break;       // Master Tune
	case 0x90: slot = kSlotGlobal + 1; break;   // Key Transpose
	case 0xa0: slot = kSlotGlobal + 2; break;   // Transpose Position
	case 0xc0: slot = kSlotGlobal + 3; break;   // Delay/Reverb
	case 0xb0:                                  // User Scale 1 / 2 key
		if (bytes.size() == 4)
			slot = m[1] == 0 ? (m[2] < 12 ? kSlotScale1 + m[2] : -1) : kSlotScale2 + (m[2] & 0x7f);
		break;
	}
	if (slot >= 0)
		post_value_locked(slot, m, bytes.size());
	else
		post_locked(m, bytes.size());
}

void Synth::note_on(int note, int velocity)
{
	std::lock_guard<std::mutex> lk(mu_);
	const std::uint8_t m[3] = { 0x10, std::uint8_t(note & 0x7f), std::uint8_t(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity)) };
	post_locked(m, 3);
	if (!key_down_[note & 0x7f]) { key_down_[note & 0x7f] = true; keys_down_++; }
}

void Synth::note_off(int note, int velocity)
{
	std::lock_guard<std::mutex> lk(mu_);
	const std::uint8_t m[3] = { 0x00, std::uint8_t(note & 0x7f), std::uint8_t(velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity)) };
	post_locked(m, 3);
	if (key_down_[note & 0x7f]) { key_down_[note & 0x7f] = false; keys_down_--; }
}

const std::uint8_t *Synth::arp_pattern(int i) const
{
	return have_arp_patterns_ && i >= 0 && i < 10 ? arp_patterns_[i].data() : nullptr;
}

void Synth::controller(int id, int value16)
{
	std::lock_guard<std::mutex> lk(mu_);
	controller_locked(id, value16);
}

void Synth::controller_locked(int id, int value16)
{
	const std::uint8_t m[4] = { 0x30, std::uint8_t(id), std::uint8_t(value16 >> 8), std::uint8_t(value16) };
	post_value_locked(id & 0x7f, m, 4);
	ctlv_[id & 0x7f] = std::uint16_t(value16);
}

Synth::Nrpn Synth::midi_cc(int cc, int value, KnobMoves *moved)
{
	Nrpn n;
	if (moved)
		moved->count = 0;
	if (cc == 120 || cc == 121 || cc == 123)   // channel mode messages, as the V55 passes them on (measured)
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (cc == 121)   // Reset All Controllers: the controller reset block of a program change
			for (const auto &r : kResetControllers)
			{
				std::uint8_t m[4];
				put_ctl(m, r[0], (r[1] << 8) | r[2]);
				post_value_locked(r[0] & 0x7f, m, 4);
				ctlv_[r[0]] = std::uint16_t((r[1] << 8) | r[2]);
			}
		else
		{
			const std::uint8_t m = cc == 120 ? 0x20 : 0x60;   // All Sound Off / All Notes Off
			post_value_locked(kSlotGlobal + (cc == 120 ? 4 : 5), &m, 1);
			key_down_.fill(false);
			keys_down_ = 0;
		}
		return n;
	}
	if (cc < 0 || cc >= 120 || cc == 0 || cc == 32)   // other channel mode / bank select are not H8 controllers
		return n;
	value = value < 0 ? 0 : (value > 127 ? 127 : value);
	std::lock_guard<std::mutex> lk(mu_);
	switch (cc)
	{
	case 99: nrpn_msb_ = value; return n;
	case 98: nrpn_lsb_ = value; return n;
	case 100: case 101: nrpn_msb_ = nrpn_lsb_ = 0x7f; return n;   // RPN: no NRPN selected
	case 96: case 97:
		if (nrpn_msb_ != 0x7f || nrpn_lsb_ != 0x7f)
			n = { nrpn_msb_ << 7 | nrpn_lsb_, 0, cc == 96 ? 1 : -1 };
		break;
	case 6:
		if (nrpn_msb_ != 0x7f || nrpn_lsb_ != 0x7f)
			n = { nrpn_msb_ << 7 | nrpn_lsb_, value, 0 };
		controller_locked(0x10 + cc, value * 32767 / 127);
		break;
	default:
		controller_locked(0x10 + cc, value * 32767 / 127);
		break;
	}
	nrpn_switch_locked(n);
	for (int k = 0; k < pe::kKnobs; k++)   // the Performance Editor knobs assigned to this CC
		if (knob_ctl_[k] == 5 + cc)
		{
			pe::Edit e[pe::kSets];
			const int ne = perf_knob_locked(k, value, e, false);
			if (moved)
			{
				auto &m = moved->m[moved->count++];
				m.knob = k;
				m.value = value;
				m.n = ne;
				for (int i = 0; i < ne; i++) m.e[i] = e[i];
			}
		}
	return n;
}

void Synth::set_knob_assign(int knob, int code)
{
	if (knob < 0 || knob >= pe::kKnobs || code < 0 || code > 100)
		return;
	std::lock_guard<std::mutex> lk(mu_);
	if (knob_ctl_[knob] == code)
		return;
	if (knob_ctl_[knob] >= 5)
		controller_locked(0x10 + knob_ctl_[knob] - 5, 0);   // the V55 resets the CC the knob had
	knob_ctl_[knob] = code;
}

int Synth::knob_assign(int knob) const
{
	std::lock_guard<std::mutex> lk(mu_);
	return knob >= 0 && knob < pe::kKnobs ? knob_ctl_[knob] : 0;
}

int Synth::controller_value(int id) const
{
	std::lock_guard<std::mutex> lk(mu_);
	const int v = ctlv_[id & 0x7f];
	return (id & 0x7f) == 0x0e ? int(std::int16_t(v)) : v;
}

void Synth::match_controllers(const Synth &other)
{
	if (&other == this)
		return;
	std::array<std::uint16_t, 128> v;
	{
		std::lock_guard<std::mutex> lk(other.mu_);
		v = other.ctlv_;
	}
	std::lock_guard<std::mutex> lk(mu_);
	for (int id = 0; id < 128; id++)
		if (ctlv_[size_t(id)] != v[size_t(id)])
			controller_locked(id, v[size_t(id)]);
	bend_ = other.bend_;
	pressure_ = other.pressure_;
}

void Synth::nrpn_switch_locked(const Nrpn &n)
{
	if (n.number < 6 || n.number > 9)
		return;
	const int bit = 1 << (n.number - 6);
	const bool on = n.step != 0 ? n.step > 0 : n.value >= 0x40;
	pe_sw_ = std::uint8_t(on ? (pe_sw_ | bit) : (pe_sw_ & ~bit));
}

int Synth::perf_knob(int knob, int value, pe::Edit out[pe::kSets])
{
	if (knob < 0 || knob >= pe::kKnobs)
		return 0;
	value = value < 0 ? 0 : (value > 127 ? 127 : value);
	std::lock_guard<std::mutex> lk(mu_);
	return perf_knob_locked(knob, value, out, true);
}

int Synth::perf_knob_locked(int knob, int value, pe::Edit out[pe::kSets], bool send_cc)
{
	if (send_cc && knob_ctl_[knob] >= 5)   // the knob is also its assigned CC to the H8 (default CC19..23)
		controller_locked(0x10 + knob_ctl_[knob] - 5, value * 32767 / 127);
	if (knob_[knob] == value || !have_perf_)
		return 0;   // the V55 applies the PE sets only when the knob position changes
	knob_[knob] = std::uint8_t(value);
	pe::Edit e[pe::kSets];
	const int n = pe::knob(perf_, pe_sw_, knob, value, curve_, edit_, e);
	int done = 0;
	for (int i = 0; i < n; i++)
		if (edit_locked(e[i].index, e[i].value))   // a model parameter of an inactive model is dropped
			out[done++] = e[i];
	return done;
}

void Synth::set_perf_position(int knob, int value)
{
	if (knob < 0 || knob >= pe::kKnobs)
		return;
	std::lock_guard<std::mutex> lk(mu_);
	knob_[knob] = std::uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

void Synth::pe_switch(int set, bool on)
{
	if (set < 0 || set >= pe::kSets)
		return;
	std::lock_guard<std::mutex> lk(mu_);
	pe_sw_ = std::uint8_t(on ? (pe_sw_ | (1 << set)) : (pe_sw_ & ~(1 << set)));
}

int Synth::perf_knob_value(int knob) const
{
	std::lock_guard<std::mutex> lk(mu_);
	return knob >= 0 && knob < pe::kKnobs ? knob_[knob] : 0;
}

bool Synth::pe_switch_on(int set) const
{
	std::lock_guard<std::mutex> lk(mu_);
	return set >= 0 && set < pe::kSets && ((pe_sw_ >> set) & 1);
}

int Synth::program_extra(int offset) const
{
	std::lock_guard<std::mutex> lk(mu_);
	return offset >= kExtraFirst && offset <= kExtraLast ? cur_dump_[offset] : 0;
}

void Synth::set_program_extra(int offset, int value)
{
	if (offset < kExtraFirst || offset > kExtraLast)
		return;
	const std::uint8_t v = std::uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value));
	std::lock_guard<std::mutex> lk(mu_);
	if (cur_dump_[offset] == v)
		return;
	if (offset <= 441 && assign_id(cur_dump_[offset]) >= 0)   // a controller assignment: reset the old one
		controller_locked(assign_id(cur_dump_[offset]), 0);
	cur_dump_[offset] = v;
	if (offset < 444)
		assign_[offset - 435] = v;
	else if (perf_.valid)   // Performance Editor Define: the knob routine reads it from the next move
	{
		const int s = (offset - 444) / 20, k = ((offset - 444) % 20) / 4;
		const std::uint8_t *a = &cur_dump_[444 + 20 * s + 4 * k];
		perf_.a[s][k] = { a[0], a[1], a[2], a[3] };
	}
}

pe::Program Synth::program_perf(int p) const
{
	return p >= 0 && p < int(programs_.size()) ? programs_[p].perf : pe::Program{};
}

int Synth::program_knob_match(int p, int knob) const
{
	if (p < 0 || p >= int(programs_.size()) || knob < 0 || knob >= pe::kKnobs || !have_perf_ || !programs_[p].perf.valid)
		return -1;
	const Program &pr = programs_[p];
	prm::Record r = pr.record;   // the edit buffer program(p) leaves (record + its PE values)
	for (const auto &e : pr.edits)
		apply_edit(r, e.data());
	const int stored = pr.perf.knob[knob];
	int best = -1;
	double best_dev = 0;
	for (int x = 0; x < 128; x++)
	{
		pe::Edit e[pe::kSets];
		const int n = pe::knob(pr.perf, pr.perf.sw, knob, x, curve_, r, e);
		double dev = 0;
		for (int i = 0; i < n; i++)   // what edit_locked would send: active parameters that change
		{
			const prm::Def &d = prm::kDefs[e[i].index];
			if (!prm::active(r, d))
				continue;
			const int diff = std::abs(e[i].value - prm::get(r, d));
			dev += double(diff) / double(d.hi - d.lo + 1);
		}
		if (best < 0 || dev < best_dev || (dev == best_dev && std::abs(x - stored) < std::abs(best - stored)))
		{
			best = x;
			best_dev = dev;
		}
	}
	return best;
}

void Synth::ramp(int id, int &last, int target)
{
	// The V55 sends four interpolated steps and then the final value (measured on the oracle).
	for (int k = 1; k <= 4; k++)
	{
		const int v = last + (target - last) * k / 4;
		controller(id, v & 0xffff);
	}
	last = target;
}

void Synth::pitch_bend(int value14)
{
	value14 = value14 < 0 ? 0 : (value14 > 16383 ? 16383 : value14);
	int v = (value14 - 8192) * 4;
	if (value14 == 16383)
		v = 0x7ffe;
	ramp(0x0e, bend_, v);
}

void Synth::aftertouch(int value, KnobMoves *moved)
{
	if (moved)
		moved->count = 0;
	value = value < 0 ? 0 : (value > 127 ? 127 : value);
	const int prev = pressure_ * 127 / 32767;
	ramp(0x0f, pressure_, value * 32767 / 127);
	// knobs assigned to aftertouch take the first ramp step only (as the oracle's V55 does)
	const int first = prev + (value - prev) / 4;
	std::lock_guard<std::mutex> lk(mu_);
	for (int k = 0; k < pe::kKnobs; k++)
		if (knob_ctl_[k] == 4)
		{
			pe::Edit e[pe::kSets];
			const int ne = perf_knob_locked(k, first, e, false);
			if (moved)
			{
				auto &m = moved->m[moved->count++];
				m.knob = k;
				m.value = first;
				m.n = ne;
				for (int i = 0; i < ne; i++) m.e[i] = e[i];
			}
		}
}

void Synth::apply_edit(prm::Record &r, const std::uint8_t *msg) const
{
	const int i = prm::index_of((msg[1] << 8) | msg[2]);
	if (i < 0)
		return;
	const prm::Def &d = prm::kDefs[i];
	prm::put(r, d, d.lo < 0 ? int(std::int8_t(msg[3])) : int(msg[3]));
}

void Synth::track_controllers(const std::vector<std::uint8_t> &burst)
{
	for (std::size_t k = 0; k < burst.size();)
	{
		const std::uint8_t *m = &burst[k];
		const int len = message_length(m, burst.size() - k);
		if (m[0] == 0x30 && k + 4 <= burst.size())
			ctlv_[m[1] & 0x7f] = std::uint16_t((m[2] << 8) | m[3]);
		k += len;
	}
}

void Synth::program(int index)
{
	if (index < 0 || index >= int(programs_.size()))
		return;
	const Program &p = programs_[index];
	std::lock_guard<std::mutex> lk(mu_);
	const std::uint8_t *b = p.bytes.data();
	std::size_t nb = p.bytes.size();
	if (p.has_dump)
	{
		nb = build_burst(p, assign_.data(), burst_.data());   // fixed buffer: no allocation
		b = burst_.data();
		std::memcpy(assign_.data(), &p.dump[435], assign_.size());
		cur_dump_ = p.dump;
	}
	post_locked(b, nb);
	edit_ = p.record;
	for (const auto &e : p.edits)
		apply_edit(edit_, e.data());
	for (std::size_t k = 0; k < nb;)
	{
		const std::uint8_t *m = b + k;
		const int len = message_length(m, nb - k);
		if (m[0] == 0x30 && k + 4 <= nb)
			ctlv_[m[1] & 0x7f] = std::uint16_t((m[2] << 8) | m[3]);
		k += len;
	}
	use_program_perf_locked(p);
}

int Synth::param(int index) const
{
	if (index < 0 || index >= prm::kCount)
		return 0;
	const prm::Def &d = prm::kDefs[index];
	std::lock_guard<std::mutex> lk(mu_);
	if (prm::active(edit_, d) || !have_defaults_)
		return prm::get(edit_, d);
	// an inactive model has no settings in the record: show what selecting it would load
	prm::Record t = edit_;
	std::memcpy(&t[prm::kSettingOffset[d.slot]], model_defaults_[d.model].data(), prm::kModelBytes);
	return std::clamp(prm::get(t, d), int(d.lo), int(d.hi));
}

bool Synth::param_active(int index) const
{
	if (index < 0 || index >= prm::kCount)
		return false;
	std::lock_guard<std::mutex> lk(mu_);
	return prm::active(edit_, prm::kDefs[index]);
}

void Synth::set_param(int index, int raw)
{
	if (index < 0 || index >= prm::kCount)
		return;
	const prm::Def &d = prm::kDefs[index];
	raw = std::clamp(raw, int(d.lo), int(d.hi));
	std::lock_guard<std::mutex> lk(mu_);
	if (d.id == prm::kOscSetId)
	{
		set_osc_locked(raw);
		return;
	}
	edit_locked(index, raw);
}

bool Synth::edit_locked(int index, int raw)
{
	const prm::Def &d = prm::kDefs[index];
	if (!prm::active(edit_, d) || prm::get(edit_, d) == raw)
		return false;
	prm::put(edit_, d, raw);
	const std::uint8_t m[4] = { 0x40, std::uint8_t(d.id >> 8), std::uint8_t(d.id), std::uint8_t(raw) };
	post_value_locked(kSlotEdit + index, m, 4);
	return true;
}

void Synth::set_osc_locked(int osc_set)
{
	const int old = edit_[prm::kOscSetOffset];
	if (old == osc_set)
		return;
	edit_[prm::kOscSetOffset] = std::uint8_t(osc_set);
	for (int slot = 1; slot <= 2; slot++)
	{
		const int m0 = prm::model_of(old, slot), m1 = prm::model_of(osc_set, slot);
		if (m1 != prm::kNone && m1 != m0 && have_defaults_)
			std::memcpy(&edit_[prm::kSettingOffset[slot]], model_defaults_[m1].data(), prm::kModelBytes);
	}
	// What the V55 sends: the whole record as a type-5 message, then its current controllers.
	const std::uint8_t hdr[3] = { 0x50, std::uint8_t(prm::kRecordSize >> 8), std::uint8_t(prm::kRecordSize) };
	post_locked(hdr, 3);
	push_locked(edit_.data(), edit_.size());
	std::uint8_t c[64];
	std::size_t n = wheel_block(assign_.data(), c);   // the program's wheels at their panel positions
	for (std::uint8_t id : kEffectControllers)
		n += put_ctl(c + n, id, ctlv_[id]);
	n += put_ctl(c + n, 0x51, ctlv_[0x51]);
	push_locked(c, n);
	for (std::size_t k = 0; k < n; k += 4)
		ctlv_[c[k + 1] & 0x7f] = std::uint16_t((c[k + 2] << 8) | c[k + 3]);
}

std::vector<int> Synth::program_params(int p) const
{
	std::vector<int> v(prm::kCount, 0);
	if (p < 0 || p >= int(programs_.size()))
		return v;
	prm::Record r = programs_[p].record;
	for (const auto &e : programs_[p].edits)
		apply_edit(r, e.data());
	for (int i = 0; i < prm::kCount; i++)
	{
		const prm::Def &d = prm::kDefs[i];
		if (prm::active(r, d) || !have_defaults_)
		{
			v[i] = prm::get(r, d);
			continue;
		}
		prm::Record t = r;   // what an OSC Set change to this model would load into the slot
		std::memcpy(&t[prm::kSettingOffset[d.slot]], model_defaults_[d.model].data(), prm::kModelBytes);
		v[i] = prm::get(t, d);
	}
	for (int i = 0; i < prm::kCount; i++)   // stay inside the declared ranges (a few factory values are not)
		v[i] = std::clamp(v[i], int(prm::kDefs[i].lo), int(prm::kDefs[i].hi));
	return v;
}

bool Synth::program_porta(int p) const
{
	return p >= 0 && p < int(programs_.size()) && programs_[p].porta;
}

void Synth::set_transpose(int semitones)
{
	semitones = std::clamp(semitones, -12, 12);
	send({ 0x90, std::uint8_t(std::int8_t(semitones)) });
}

void Synth::set_master_tune(int tenths_hz)
{
	tenths_hz = std::clamp(tenths_hz, -100, 100);
	send({ 0x80, std::uint8_t(std::int8_t(tenths_hz)) });
}

void Synth::set_transpose_position(int pos) { send({ 0xa0, std::uint8_t(pos ? 1 : 0) }); }

void Synth::set_delay_reverb(bool on) { send({ 0xc0, std::uint8_t(on ? 1 : 0) }); }

void Synth::set_user_scale(int scale, int key, int cents)
{
	if (scale < 1 || scale > 2 || key < 0 || key >= (scale == 1 ? 12 : 128))
		return;
	cents = std::clamp(cents, -100, 100);
	send({ 0xb0, std::uint8_t(scale - 1), std::uint8_t(key), std::uint8_t(std::int8_t(cents)) });
}

void Synth::render(float *left, float *right, int n)
{
	const auto t0 = std::chrono::steady_clock::now();
	if (idle_sleep_ && sleeping_)
	{
		bool wake;
		{
			std::lock_guard<std::mutex> lk(mu_);
			wake = tail_ != head_;   // (post_locked has woken it already; held messages wait)
			if (wake)
				wake_locked();
		}
		if (!wake)
		{
			for (int i = 0; i < n; i++) left[i] = right[i] = 0.0f;
			load_ = 0;
			return;
		}
	}
	float peak = 0;
	for (int i = 0; i < n; i++)
	{
		if (byte_gap_ > 0)
			byte_gap_--;
		else
		{
			int b = -1;
			{
				std::lock_guard<std::mutex> lk(mu_);
				if (tail_ != head_)
				{
					b = ring_[tail_];
					tail_ = (tail_ + 1) & (ring_.size() - 1);
				}
			}
			if (b >= 0)
			{
				ctl_->uart_byte(std::uint8_t(b));
				byte_gap_ = kFramesPerUartByte;
			}
		}
		if (frame_ % ctl::H8Control::kFramesPerTick == 0)
			ctl_->tick();
		ctl_->pump_dsp();
		std::int32_t l, r;
		board_->frame(l, r);
		left[i] = float(l) / 8388608.0f;
		right[i] = float(r) / 8388608.0f;
		const float a = std::fabs(left[i]) > std::fabs(right[i]) ? std::fabs(left[i]) : std::fabs(right[i]);
		peak = a > peak ? a : peak;
		frame_++;
	}
	if (idle_sleep_)
	{
		std::lock_guard<std::mutex> lk(mu_);
		if ((peak < 1e-6f || muted_) && keys_down_ == 0 && tail_ == head_ && byte_gap_ == 0)
			silent_frames_ += n;
		else
			silent_frames_ = 0;
		if (silent_frames_ >= kSleepFrames)
			sleeping_.store(true, std::memory_order_relaxed);
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	load_ = 100.0 * secs / (double(n) / kSampleRate);
}

} // namespace kp
