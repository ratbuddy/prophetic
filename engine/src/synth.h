// Prophecy synth facade: bit-exact DSP board + translated H8 control layer, driven with
// V55-format messages (the protocol the panel/MIDI CPU speaks to the H8):
//   note on  = 10 nn vv      note off = 00 nn 00      controller = 30 id hi lo
//   program  = the captured V55 burst for that program (controllers + 435-byte record + the
//              program's Performance Editor values as parameter edits)
//   parameter edit = 40 hh ll vv (control/params.h)       key transpose = 90 vv
// All firmware-derived inputs (ROM, initial snapshot, program bursts) are loaded at run time
// from a data directory produced locally from the user's ROM (see tools/, docs/BYO-ROM.md).
#pragma once

#include "control/h8ctl.h"
#include "control/params.h"
#include "control/perf.h"
#include "control/rom_view.h"
#include "prophecy_board.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kp {

class Synth
{
public:
	static constexpr int kSampleRate = 48000;

	// dir must contain ic22.bin (user ROM, word-swapped dump as dumped) and either ic12.bin (the
	// V55 ROM: the factory programs, oscillator-model defaults, Performance Editor curve, arpeggio
	// patterns; with it and without a snapshot the engine boots the firmware natively, see
	// boot_native) or a snapshot state.bin (board) + ram.bin (H8 RAM at the same frame), which
	// skips the boot (tools/native_boot writes one); programs/NNN.uart + NNN.name: captured bursts,
	// used only without ic12.bin.
	// native_boot: boot the firmware natively even when state.bin / ram.bin exist (tools).
	bool load(const std::string &dir, std::string &error, bool native_boot = false);

	// Thread-safe event entry points (UI / MIDI / audio threads); nothing here allocates.
	void note_on(int note, int velocity);
	void note_off(int note, int velocity = 0);   // release velocity (the V55 forwards it: `00 nn vv`)
	void program(int index);   // 0-63 bank A, 64-127 bank B
	void controller(int id, int value16);

	// MIDI, mapped the way the V55 does (measured on the oracle):
	//   CC n -> controller id 0x10+n, value v*32767/127, except bank select (0, 32) and the NRPN/RPN
	//   selectors and data increment/decrement (96..101), which the V55 keeps; CC19..23 also turn
	//   the Performance Editor knobs 1..5 (perf_knob); pitch bend -> id 0x0E, signed (bend-8192)*4;
	//   All Sound Off (CC120) -> `20`, All Notes Off (CC123) -> `60`, Reset All Controllers (CC121) ->
	//   the controller reset block;
	//   channel aftertouch -> id 0x0F, v*32767/127. Bend/aftertouch ramp in 4 steps like the V55.
	// NRPN (Parameter Guide 2-1): 00 01..00 05 arpeggiator, 00 06..00 09 PE1..PE4 on/off, set by data
	// entry (CC6, still forwarded as a controller) or data increment/decrement (CC96/97; on a switch
	// increment = on, decrement = off); an RPN selection (CC100/101) deselects. midi_cc() applies
	// the PE switches itself and returns every NRPN change so the caller can apply the arpeggiator.
	struct Nrpn
	{
		int number = -1;   // -1: this CC set no NRPN parameter
		int value = 0;     // data entry value (step == 0)
		int step = 0;      // +1 / -1: data increment / decrement
	};
	// Knobs a MIDI message turned (Global Knob Controller Assign) and the edits they made.
	struct KnobMoves
	{
		int count = 0;
		struct Move { int knob, value, n; pe::Edit e[pe::kSets]; } m[pe::kKnobs];
	};
	Nrpn midi_cc(int cc, int value, KnobMoves *moved = nullptr);
	void pitch_bend(int value14);
	void aftertouch(int value, KnobMoves *moved = nullptr);

	// Global Knob Controller Assign (0 Off, 1 PBend+/-, 2 PBend+, 3 PBend-, 4 ATouch, 5 + n CC#n;
	// default CC19..23), as the V55 uses it on MIDI input (measured on the oracle): a CC turns every
	// knob assigned to it (after being forwarded as a controller, once); aftertouch moves its knobs
	// to the first step of the V55's 4-step aftertouch ramp only; pitch bend moves none.
	// Reassigning a knob resets the CC it had to 0 (`30 id 00 00`).
	void set_knob_assign(int knob, int code);
	int knob_assign(int knob) const;
	int controller_value(int id) const;   // last value sent for an H8 controller id (0x0E bend: signed)

	// Performance Editor (needs ic12.bin: the factory programs' PE settings and the V55's curve
	// table). Knobs 0..4 = PE knobs 1..5 (0..127), sets 0..3 = PE1..PE4. perf_knob() is what turning
	// a PE knob does: its controller (0x23 + knob) goes to the H8 and the switched-on sets assigned
	// to that knob edit their parameters (control/perf.h), applied to the edit buffer and sent like
	// any edit. Returns the edits made (kDefs index + value) so a UI can show them.
	int perf_knob(int knob, int value, pe::Edit out[pe::kSets]);   // sends the knob's assigned CC
	void set_perf_position(int knob, int value);   // position only, nothing applied (project restore)
	void pe_switch(int set, bool on);               // applies from the next knob move, like the V55
	int perf_knob_value(int knob) const;
	bool pe_switch_on(int set) const;
	bool has_perf() const { return have_perf_; }
	pe::Program program_perf(int p) const;          // program p's stored settings (valid false: none)

	// Program data only the V55 keeps (the H8 never sees it), edited like a program parameter (the
	// loaded program's copy; a program change reloads the stored one): Controller Assignment (dump
	// bytes 435..443: Wheel1, Wheel2, Wheel3+, Wheel3-, Ribbon X, Ribbon Z, Foot Pedal = 0 Off, 1-3
	// PBend+/-, +, -, 4 ATouch, 5 + n CC#n; Foot SW 0..11; Ribbon X centre 0/1) and Performance Editor
	// Define (444..523: per set and knob {PE number, left %, right %, curve}). As measured on the
	// oracle: reassigning a wheel / ribbon / pedal resets the controller it had to 0; Define edits
	// send nothing and apply from the next knob move (as a panel edit does).
	static constexpr int kExtraFirst = 435, kExtraLast = 523;
	int program_extra(int offset) const;
	void set_program_extra(int offset, int value);

	// The Prophecy's 10 internal arpeggio patterns (UP, DOWN, ALT1, ALT2, RANDOM, PAT1-5) as
	// stored in the V55 ROM, 128 bytes each (Arp::PatternData::from_bytes); nullptr without ic12.bin.
	const std::uint8_t *arp_pattern(int i) const;

	// Program parameters (index into prm::kDefs). The edit buffer is the current program record as
	// the H8 has it; an edit changes it and sends the V55's parameter-change message. Edits of
	// oscillator-model parameters for an inactive model are dropped (as the V55 does); OSC Set
	// re-sends the program with changed slots reset to the model defaults.
	int param(int index) const;   // inactive model parameters: that model's defaults
	void set_param(int index, int raw);
	bool param_active(int index) const;
	// Parameter values right after program(p): the record plus its Performance Editor values;
	// inactive model parameters show that model's defaults. porta: the program's CC65 state.
	std::vector<int> program_params(int p) const;
	bool program_porta(int p) const;
	bool has_model_defaults() const { return have_defaults_; }

	// Global settings (not stored per program), as the V55 sends them on a Global parameter change
	// (measured on the oracle; the velocity / aftertouch curves and AT sense only shape the
	// Prophecy's own keyboard, MIDI input reaches the H8 unchanged).
	void set_transpose(int semitones);                    // -12..+12: `90 vv`
	void set_master_tune(int tenths_hz);                  // (Hz - 440) * 10, -100..+100 (430..450 Hz): `80 vv`
	void set_delay_reverb(bool on);                       // Delay/Reverb on or bypassed: `c0 01` / `c0 00`
	void set_user_scale(int scale, int key, int cents);   // User Scale 1 (keys 0..11) / 2 (0..127), -100..+100: `b0 tt kk vv`
	void set_transpose_position(int pos);                 // 0 Post-KBD, 1 Pre-TG: `a0 vv`

	// Audio thread: render n frames of 48 kHz stereo (float, roughly -1..1).
	void render(float *left, float *right, int n);

	// Idle sleep (off by default; the tests compare continuous runs): after kSleepFrames of output
	// below -120 dBFS with no key held and nothing waiting for the H8, the whole engine (H8 and the
	// three DSPs) stops and render() outputs silence; the next message wakes it and it continues
	// from where it stopped, as if the silent stretch had not happened (free-running LFOs resume
	// where they were).
	static constexpr int kSleepFrames = kSampleRate;   // 1 s
	void set_idle_sleep(bool on) { idle_sleep_ = on; }
	bool sleeping() const { return sleeping_; }
	// The caller's output is muted (the plugin's Stop fade): idle sleep counts the engine's output as
	// silence, so a sound the Prophecy itself keeps ringing (All Sound Off does not end every
	// program's decay) no longer keeps the engine awake. Audio thread.
	void set_muted(bool on) { muted_ = on; }
	std::uint64_t emulated_frames() const { return frame_; }   // frames the engine has actually run

	int program_count() const { return int(programs_.size()); }
	// Add a program from a 535-byte dump (e.g. from a SysEx file, control/sysex.h); call before the
	// audio thread runs. Returns its index. label prefixes its name.
	int add_program(const std::uint8_t *dump, const std::string &label);
	bool program_dump(int index, std::uint8_t *out) const;   // its 535-byte dump (false: none)
	// Load every Prophecy SysEx file (*.syx) in dir, in name order: their programs are added as
	// "Unnn <name>" (program dumps of any kind, control/sysex.h); arpeggio patterns they contain
	// replace the ROM's (arp_pattern). Call before the audio thread runs. Returns the programs added.
	int load_user_sysex(const std::string &dir, std::string *report = nullptr);
	// The V55 burst program(index) sends after a program with Controller Assignment prev_assign
	// (9 bytes; nullptr: the boot program A00's). Diagnostics / tests.
	std::vector<std::uint8_t> program_burst(int index, const std::uint8_t *prev_assign = nullptr) const;
	const std::string &program_name(int i) const { return programs_[i].name; }
	double load_percent() const { return load_; }   // render time / real time, last block
	ProphecyBoard &board() { return *board_; }       // diagnostics / tools
	// The H8 RAM in ram.bin's layout (tools/native_boot: state.bin + ram.bin from a native boot).
	std::vector<std::uint8_t> h8_ram() const;
	// load() booted the firmware natively (no snapshot in its dir).
	bool booted() const { return booted_; }
	// Write the engine as it is now as state.bin + ram.bin into dir (after a native boot: the next
	// load() skips the boot). Not on the audio thread. False if a file could not be written.
	bool save_snapshot(const std::string &dir) const;
	// Frame of a native boot at which the engine stops booting and plays (Synth::load).
	static constexpr std::uint64_t kBootFrames = 2683ull * 195;   // 10.9 s, on an H8 tick

private:
	struct Program
	{
		std::string name;
		std::vector<std::uint8_t> bytes;                     // the burst as the V55 sends it
		prm::Record record{};                                // its program record
		std::vector<std::array<std::uint8_t, 4>> edits;      // its trailing parameter edits (PE values)
		bool porta = false;
		pe::Program perf;                                    // its Performance Editor settings
		std::array<std::uint8_t, pe::kDumpSize> dump{};      // its 535-byte dump (burst built from it)
		bool has_dump = false;
	};
	static constexpr std::size_t kBurstMax = 1024;
	Program program_from_dump(const std::uint8_t *dump, const std::string &label) const;
	std::size_t build_burst(const Program &p, const std::uint8_t *prev_assign, std::uint8_t *out) const;
	std::size_t pe_edits(const Program &p, std::uint8_t *out) const;
	void boot_native();
	void run_frames(std::uint64_t until);
	void run_until_sent();

	void send(std::initializer_list<std::uint8_t> bytes);
	void push_locked(const std::uint8_t *p, std::size_t n);
	bool edit_locked(int index, int raw);   // edit buffer + `40` message; false if nothing to do
	void controller_locked(int id, int value16);
	void use_program_perf_locked(const Program &p);
	void nrpn_switch_locked(const Nrpn &n);
	int perf_knob_locked(int knob, int value, pe::Edit out[pe::kSets], bool send_cc);
	void apply_edit(prm::Record &r, const std::uint8_t *msg) const;
	void track_controllers(const std::vector<std::uint8_t> &burst);
	void set_osc_locked(int osc_set);
	void ramp(int id, int &last, int target);

	ctl::RomView rom_;
	std::unique_ptr<ProphecyBoard> board_;
	std::unique_ptr<ctl::H8Control> ctl_;
	std::vector<Program> programs_;
	mutable std::mutex mu_;
	// V55 -> H8 bytes waiting to be sent (paced at the UART rate); fixed ring, no allocation.
	std::array<std::uint8_t, 1 << 16> ring_{};
	std::size_t head_ = 0, tail_ = 0;
	prm::Record edit_{};
	std::array<std::array<std::uint8_t, prm::kModelBytes>, prm::kModels> model_defaults_{};
	bool have_defaults_ = false;
	std::array<std::uint16_t, 128> ctlv_{};   // last value sent per H8 controller id
	std::array<std::uint8_t, 9> assign_{};    // the loaded program's Controller Assignment
	std::array<std::uint8_t, pe::kDumpSize> cur_dump_{};   // the loaded program's dump (V55-only data edited)
	std::array<std::uint8_t, kBurstMax> burst_{};
	pe::Curve curve_{};
	bool have_perf_ = false;
	std::array<std::array<std::uint8_t, 128>, 10> arp_patterns_{};
	bool have_arp_patterns_ = false;
	pe::Program perf_{};                      // current program's Performance Editor settings
	std::uint8_t pe_sw_ = 0;
	std::array<std::uint8_t, pe::kKnobs> knob_{};
	int nrpn_msb_ = 0x7f, nrpn_lsb_ = 0x7f;   // 7F 7F = none selected
	std::array<int, pe::kKnobs> knob_ctl_{ 5 + 19, 5 + 20, 5 + 21, 5 + 22, 5 + 23 };   // Knob Controller Assign

	std::uint64_t frame_ = 0;
	int byte_gap_ = 0;
	double load_ = 0;
	bool idle_sleep_ = false, sleeping_ = false, muted_ = false;
	bool booted_ = false;
	int silent_frames_ = 0;
	std::array<bool, 128> key_down_{};   // note on without its note off yet (keeps the engine awake)
	int keys_down_ = 0;
	int bend_ = 0, pressure_ = 0;   // last values sent (signed bend, unsigned pressure)
};

} // namespace kp
