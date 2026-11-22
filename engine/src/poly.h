// Polyphony: several complete engines (kp::Synth: H8 + the three-DSP board each), one per voice.
// The Prophecy is monophonic; with voices() == 1 this is the Prophecy exactly (every call goes to
// voice 0 unchanged). With more, each note gets a voice of its own, and everything else (program,
// parameter edits, controllers, global settings) goes to every active voice, so each voice is a
// Prophecy playing that one note, bit-exact. Each voice has its own effects (DSP3): the delay and
// reverb sum to the same as one effect on the mix (they are linear), modulated and distortion
// effects run per voice. Voices that are silent sleep (Synth idle sleep) and cost nothing.
//
// Voices beyond voices() are neither rendered nor sent anything (a voice dropped by lowering
// voices() fades out over kFadeFrames first). When voices() grows the caller brings the new ones
// up to date while the others keep playing: only_from(first) sends what follows to the new voices
// only (the plugin reloads the program with its edits there and re-sends the global settings, as
// for a project load), match_controllers(first) gives them voice 0's controllers, only_from(0).
#pragma once

#include "synth.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace kp {

class PolySynth
{
public:
	static constexpr int kMaxVoices = 8;

	PolySynth();
	~PolySynth();

	// Loads voice 0 from dir (when it booted natively, its snapshot is saved into dir so the other
	// voices load it instead of booting again). Not on the audio thread.
	bool load(const std::string &dir, std::string &error);
	int load_user_sysex(const std::string &dir, std::string *report = nullptr);
	// The other voices, loaded on a background thread (each from dir, then the SysEx folder
	// load_user_sysex was given, if any): the plugin is ready with voice 0 alone, and set_voices
	// takes the voices that have arrived. Without this call voices 1.. are loaded on first use
	// by tests (load_all).
	void load_voices_async();
	void load_all();   // the same, on this thread (tests, tools)
	int loaded_voices() const { return loaded_.load(std::memory_order_acquire); }

	// Active voices (1..the loaded ones). Lowering it releases the notes of the voices dropped and
	// fades them out over kFadeFrames (no click); the voices kept play on undisturbed.
	static constexpr int kFadeFrames = Synth::kSampleRate / 50;   // 20 ms
	void set_voices(int n);
	int voices() const { return voices_; }
	// Broadcasts (the "to every active voice" calls below) reach voices first.. only, and voice
	// `first` answers the voice-0 queries, until only_from(0): for bringing added voices up to date
	// without touching the playing ones. Audio thread, like the calls it scopes.
	void only_from(int first) { from_ = first < 0 ? 0 : (first >= voices_ ? voices_ - 1 : first); }
	// Voices first.. take voice 0's controller values (Synth::match_controllers).
	void match_controllers(int first);

	// Notes: voice allocation (a free voice, the one released longest ago, a voice just added
	// counting as just released; if none is free, the one holding its note longest). One voice:
	// every note to voice 0, as the Prophecy.
	void note_on(int note, int velocity);
	void note_off(int note, int velocity = 0);
	// Polyphonic pressure: the voice playing `note` (one voice: the Prophecy's aftertouch).
	void poly_pressure(int note, int value, Synth::KnobMoves *moved = nullptr);

	// To every active voice (voice 0's result / moves; see only_from).
	void program(int index);
	void set_param(int index, int raw);
	void set_program_extra(int offset, int value);
	void set_user_scale(int scale, int key, int cents);
	void set_transpose(int semitones);
	void set_transpose_position(int pos);
	void set_master_tune(int tenths_hz);
	void set_delay_reverb(bool on);
	void set_knob_assign(int knob, int code);
	void set_perf_position(int knob, int value);
	void pe_switch(int set, bool on);
	void set_idle_sleep(bool on);
	void set_muted(bool on);
	void pitch_bend(int value14);
	void aftertouch(int value, Synth::KnobMoves *moved = nullptr);
	Synth::Nrpn midi_cc(int cc, int value, Synth::KnobMoves *moved = nullptr);
	int perf_knob(int knob, int value, pe::Edit out[pe::kSets]);

	// Voice 0's view (the voice-dependent ones: voice only_from's).
	int program_count() const { return v_[0]->program_count(); }
	const std::string &program_name(int i) const { return v_[0]->program_name(i); }
	bool program_dump(int index, std::uint8_t *out) const { return v_[0]->program_dump(index, out); }
	bool program_porta(int p) const { return v_[0]->program_porta(p); }
	pe::Program program_perf(int p) const { return v_[0]->program_perf(p); }
	int program_knob_match(int p, int knob) const { return v_[0]->program_knob_match(p, knob); }
	std::vector<int> program_params(int p) const { return v_[0]->program_params(p); }
	int program_extra(int offset) const { return v_[from_]->program_extra(offset); }
	int param(int index) const { return v_[from_]->param(index); }
	bool param_active(int index) const { return v_[from_]->param_active(index); }
	int controller_value(int id) const { return v_[from_]->controller_value(id); }
	bool pe_switch_on(int set) const { return v_[from_]->pe_switch_on(set); }
	bool has_perf() const { return v_[0]->has_perf(); }
	bool has_model_defaults() const { return v_[0]->has_model_defaults(); }
	const std::uint8_t *arp_pattern(int i) const { return v_[0]->arp_pattern(i); }
	bool booted() const { return v_[0]->booted(); }
	bool save_snapshot(const std::string &dir) const { return v_[0]->save_snapshot(dir); }

	// Audio thread: the active voices, summed.
	void render(float *left, float *right, int n);
	bool sleeping() const;   // every active voice asleep
	Synth &voice(int k) { return *v_[k]; }   // tests / diagnostics

private:
	void load_rest();
	std::array<std::unique_ptr<Synth>, kMaxVoices> v_;
	std::string dir_, user_dir_;
	std::atomic<int> loaded_{ 0 };
	std::atomic<bool> quit_{ false };
	std::thread loader_;
	int voices_ = 1;
	int from_ = 0;                                // only_from
	std::array<int, kMaxVoices> fade_{};          // frames of fade-out left (voices dropped)
	int fading_ = 0;                              // voices with fade_ > 0
	std::array<int, kMaxVoices> note_{};          // the voice's note (-1: none yet)
	std::array<bool, kMaxVoices> held_{};         // its key is down
	std::array<std::uint64_t, kMaxVoices> when_{};   // last note on / off (allocation order)
	std::uint64_t clock_ = 0;
	std::array<float, 256> mix_l_{}, mix_r_{};
};

} // namespace kp
