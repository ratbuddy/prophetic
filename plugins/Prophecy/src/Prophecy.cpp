// Prophecy.cpp - KP Sim "Prophetic": the native Korg Prophecy engine as an MPC instrument.
//
// Signal flow: MIDI -> kp::Synth (V55-format messages -> translated H8 control layer -> bit-exact
// 3-DSP board, 48 kHz) -> 32-tap windowed-sinc resampler (48 kHz -> host rate, 44.1 kHz on the MPC)
// -> volume -> both outputs.
//
// Parameters (order is permanent; append only):
//   0  Volume (dB; parameter 0 is nudged by the host, so it must stay harmless; preset changes keep it)
//   1  Engine Load (display only: the engine's worst recent render time as % of real time)
//   2.. every Prophecy program parameter that reaches the sound engine (engine/src/control/
//      params.h: 525 of them, in Parameter Guide order), raw values as the Prophecy stores them
//   then Porta SW (CC65), OSC1 Model / OSC2 Model (display only, follow OSC Set; they drive the
//      model pages' visibility), then the global settings (not part of a program): Transpose and
//      the arpeggiator (On, Pattern, Octave, Latch, KeySync, Step, Gate, Velocity, Sort, scan zone,
//      BPM when the host gives no tempo; engine/src/arp.h), then the Performance Editor: Perf Knob
//      1-5 and PE1-4 On (set by each program; turning a knob edits the parameters its switched-on
//      PE sets are assigned to, exactly as the Prophecy's V55 computes them: engine/src/control/perf.h),
//      then Master Tune (430-450 Hz), Dly/Rev On (the Global Delay/Reverb bypass) and User Scale 1
//      (12 notes, -100..+100 cents; Scale Type User1), the arpeggiator's controllers (Vel / Gate Ctrl:
//      Global Arpeggiator Controller Assign; Vel / Gate Int: the pattern's intensities), Knob1-5 Ctrl
//      (Global Knob Controller Assign: the MIDI message that turns each PE knob, default CC19-23),
//      Transpose Position and User Scale 2 (128 notes), then per program the Controller Assignment
//      (wheels, ribbon, pedal, foot switch), and the two Define editors: Performance Editor Define
//      (PE Ed Set / Knob select a slot; PE Param / Left / Right / Curve edit it) and Arpeggio Pattern
//      Define (Pat Ed Step selects a step of the selected pattern; Pat Tone / Velocity / Gate / Offset
//      edit it; Arp Type / Oct Alt). Projects save the Define edits and all 10 patterns. The velocity / aftertouch curves are not
//      needed: the Prophecy applies them only to its own keyboard, never to MIDI input.
// Programs: the 128 factory programs (bank A 0-63, bank B 64-127), named and valued from Content/.
// Editing: a parameter whose value differs from what the engine holds is sent as the V55's
// parameter edit (OSC Set first, as it resets the oscillator-model settings). Program changes
// reload the program and take its values; a project restores its program and then re-applies
// every edited value.
// MIDI: notes, CC (as the V55 maps them; CC19-23 = Perf Knob 1-5), NRPN 00 01-00 09 (arpeggiator
// pattern/on/octave/latch/key sync, PE1-4 on/off), pitch bend, channel aftertouch, bank select
// (CC0 0 + CC32 1 = bank B) + program change.
//
// Content/ (generated locally from the user's own ROM, never distributed): ic22.bin, ic12.bin,
// state.bin, ram.bin (programs/NNN.uart + NNN.name only without ic12.bin). Without it the plugin
// stays silent. Content/User/*.syx: your Prophecy SysEx libraries (programs, banks, all-data and
// arpeggio pattern dumps): their programs follow the factory ones as presets "Unnn", reachable by
// MIDI bank select CC32 = 2, 3, ... + program change.
#include "mpc_plugin.h"
#include "arp.h"
#include "poly.h"
#include "synth.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <chrono>
#if defined(__linux__)
#include <sched.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// Windowed-sinc fractional resampler (Kaiser, 32 taps, 256 phases), stereo, 48 kHz in -> any rate out.
class Resampler
{
public:
	void configure(double in_rate, double out_rate)
	{
		step_ = in_rate / out_rate;
		bypass_ = std::fabs(step_ - 1.0) < 1e-9;
		const double cutoff = 0.45 * std::fmin(1.0, out_rate / in_rate);   // cycles per input sample
		table_.assign(size_t(kPhases + 1) * kTaps, 0.0f);
		for (int p = 0; p <= kPhases; p++)
		{
			const double frac = double(p) / kPhases;
			double sum = 0;
			for (int k = 0; k < kTaps; k++)
			{
				const double x = double(k - kTaps / 2 + 1) - frac;
				const double s = x == 0 ? 2 * cutoff : std::sin(2 * M_PI * cutoff * x) / (M_PI * x);
				const double t = x / (kTaps / 2);
				const double w = std::fabs(t) >= 1 ? 0.0 : bessel0(kBeta * std::sqrt(1 - t * t)) / bessel0(kBeta);
				table_[size_t(p) * kTaps + k] = float(s * w);
				sum += s * w;
			}
			for (int k = 0; k < kTaps; k++)
				table_[size_t(p) * kTaps + k] = float(table_[size_t(p) * kTaps + k] / sum);
		}
		inL_.assign(kTaps, 0.0f);   // pre-roll: kTaps/2 samples of latency
		inR_.assign(kTaps, 0.0f);
		inL_.reserve(16384);        // no reallocation in the audio thread
		inR_.reserve(16384);
		pos_ = 0.0;
	}
	// Input frames needed before producing n more outputs. Output i reads buffer[base .. base+kTaps-1]
	// with base = floor(pos + i*step).
	int needed(int n) const
	{
		if (bypass_) return n - int(inL_.size());
		const double last = pos_ + (n - 1) * step_;
		return int(std::floor(last)) + kTaps - int(inL_.size());
	}
	void push(const float *l, const float *r, int n) { inL_.insert(inL_.end(), l, l + n); inR_.insert(inR_.end(), r, r + n); }
	void pull(float *l, float *r, int n)
	{
		if (bypass_)
		{
			for (int i = 0; i < n; i++) { l[i] = inL_[i]; r[i] = inR_[i]; }
			inL_.erase(inL_.begin(), inL_.begin() + n);
			inR_.erase(inR_.begin(), inR_.begin() + n);
			return;
		}
		for (int i = 0; i < n; i++)
		{
			const int base = int(std::floor(pos_));
			const double frac = pos_ - base;
			const float *h = &table_[size_t(frac * kPhases + 0.5) * kTaps];
			float a = 0, b = 0;
			for (int k = 0; k < kTaps; k++) { a += inL_[base + k] * h[k]; b += inR_[base + k] * h[k]; }
			l[i] = a;
			r[i] = b;
			pos_ += step_;
		}
		const int drop = int(std::floor(pos_));   // keep kTaps of history before the read position
		if (drop > 0)
		{
			inL_.erase(inL_.begin(), inL_.begin() + drop);
			inR_.erase(inR_.begin(), inR_.begin() + drop);
			pos_ -= drop;
		}
	}

private:
	static constexpr int kTaps = 32, kPhases = 256;
	static constexpr double kBeta = 8.0;
	static double bessel0(double x)
	{
		double s = 1, t = 1;
		for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
		return s;
	}
	std::vector<float> table_, inL_, inR_;
	double step_ = 1.0, pos_ = 0.0;
	bool bypass_ = true;
};

const std::vector<std::string> kModelNames = { "None", "Std", "Comb", "VPM", "Mod", "Brass", "Reed", "Pluck" };

} // namespace

class Prophecy : public mpc::Plugin
{
public:
	// Fixed parameters around the Prophecy program parameters (see the file comment).
	static constexpr int kVolume = 0, kLoad = 1, kFirstProgParam = 2;
	static constexpr int kPorta = kFirstProgParam + 525;
	static constexpr int kOsc1Model = kPorta + 1, kOsc2Model = kPorta + 2;
	static constexpr int kPresetParams = kOsc2Model + 1;   // presets set parameters 0..kPresetParams-1
	static constexpr int kTranspose = kPresetParams;
	// arpeggiator (global, Performance-mode settings)
	static constexpr int kArpOn = kTranspose + 1, kArpPattern = kArpOn + 1, kArpOctave = kArpOn + 2,
	                     kArpLatch = kArpOn + 3, kArpKeySync = kArpOn + 4, kArpStep = kArpOn + 5,
	                     kArpGate = kArpOn + 6, kArpVelocity = kArpOn + 7, kArpSort = kArpOn + 8,
	                     kArpZoneLo = kArpOn + 9, kArpZoneHi = kArpOn + 10, kArpBpm = kArpOn + 11;
	// Performance Editor: knob positions and set switches (set from each program, saved with projects)
	static constexpr int kKnob1 = kArpBpm + 1;                  // Perf Knob 1..5
	static constexpr int kPeSw1 = kKnob1 + kp::pe::kKnobs;      // PE1..PE4
	// more global settings (Prophecy Global mode)
	static constexpr int kMasterTune = kPeSw1 + kp::pe::kSets;  // 430.0..450.0 Hz
	static constexpr int kDlyRev = kMasterTune + 1;             // Delay/Reverb on (off = bypassed)
	static constexpr int kUserScale1 = kDlyRev + 1;             // User Scale 1, C..B, -100..+100 cents
	static constexpr int kArpVelCtrl = kUserScale1 + 12;        // Global Arpeggiator Controller Assign: velocity
	static constexpr int kArpGateCtrl = kArpVelCtrl + 1;        //   and gate (Off, PBend, ATouch, CC#0..95)
	static constexpr int kArpVelInt = kArpGateCtrl + 1;         // the pattern's Velocity / Gate Controller Intensity
	static constexpr int kArpGateInt = kArpVelInt + 1;
	static constexpr int kKnobCtl1 = kArpGateInt + 1;           // Global Knob Controller Assign, knobs 1..5
	static constexpr int kTransposePos = kKnobCtl1 + kp::pe::kKnobs;   // Post-KBD / Pre-TG
	static constexpr int kUserScale2 = kTransposePos + 1;       // User Scale 2, C-1..G9, -100..+100 cents
	// per program (V55-only data): Controller Assignment of the wheels, ribbon, pedal and foot switch
	static constexpr int kCtlAssign1 = kUserScale2 + 128;       // 9: Wheel1..Pedal, Foot SW, RibX Center
	// editors (as the Prophecy's Define modes): a selection, then the selected slot's fields
	static constexpr int kPeEdSet = kCtlAssign1 + 9, kPeEdKnob = kPeEdSet + 1;   // Performance Editor Define
	static constexpr int kPeParam = kPeEdSet + 2, kPeLeft = kPeEdSet + 3, kPeRight = kPeEdSet + 4, kPeCurve = kPeEdSet + 5;
	static constexpr int kPatEdStep = kPeCurve + 1;              // Arpeggio Pattern Define (the selected pattern)
	static constexpr int kPatTone = kPatEdStep + 1, kPatVel = kPatEdStep + 2, kPatGate = kPatEdStep + 3,
	                     kPatOffset = kPatEdStep + 4, kArpType = kPatEdStep + 5, kArpOctAlt = kPatEdStep + 6;
	static constexpr int kMidiIn1 = kArpOctAlt + 1;              // display only: the last 3 MIDI messages received
	static constexpr int kMidiLog = kMidiIn1 + 3;                // on: every MIDI message received goes to Content/midi_log.txt
	static constexpr int kIdleSleep = kMidiLog + 1;              // stop the engine during silence (saves CPU)
	static constexpr int kVoices = kIdleSleep + 1;               // polyphony: engines, one per voice (1 = the Prophecy)
	static constexpr int kNumParams = kVoices + 1;

	static mpc::PluginInfo makeInfo()
	{
		mpc::PluginInfo i;
		i.name = "Prophetic";   // must match "name" in plugin.json (the internal names stay Prophecy)
		i.vendor = "KP Sim";
		i.uniqueId = MPC_FOURCC('K', 'p', 'P', 'r');   // must match "uid" in plugin.json
		i.version = 110;
		i.isInstrument = true;
		i.numInputs = 0;
		i.numOutputs = 2;
		return i;
	}

	Prophecy() : mpc::Plugin(makeInfo())
	{
		static_assert(kPorta == kFirstProgParam + 525, "parameter table size changed: parameter order is permanent");
		addParam(mpc::floatParam("vol", "Volume", -24.0f, 24.0f, 12.0f, "dB", 1));
		{
			mpc::Param p = mpc::floatParam("load", "Engine Load", 0.0f, 100.0f, 0.0f, "%", 0);
			p.automatable = true;   // the MPC leaves non-automatable parameters off its pages
			addParam(p);
		}
		for (int i = 0; i < kp::prm::kCount; i++)
		{
			const kp::prm::Def &d = kp::prm::kDefs[i];
			if (d.fmt == kp::prm::Fmt::Choice)
			{
				std::vector<std::string> c(d.list, d.list + d.nlist);
				addParam(mpc::choiceParam(d.key, d.name, c, 0));
			}
			else
				addParam(mpc::intParam(d.key, d.name, d.lo, d.hi, d.lo < 0 ? 0 : d.lo));
		}
		addParam(mpc::boolParam("porta", "Porta SW", false));
		for (int s = 1; s <= 2; s++)
		{
			mpc::Param p = mpc::choiceParam(s == 1 ? "osc1model" : "osc2model", s == 1 ? "OSC1 Model" : "OSC2 Model", kModelNames, 0);
			p.automatable = true;   // the MPC leaves non-automatable parameters off its pages
			addParam(p);
		}
		addParam(mpc::intParam("transpose", "Transpose", -12, 12, 0, "st"));
		addParam(mpc::boolParam("arp_on", "Arp On", false));
		addParam(mpc::choiceParam("arp_pattern", "Arp Pattern", { "Up", "Down", "Alt1", "Alt2", "Random", "Pat1", "Pat2", "Pat3", "Pat4", "Pat5" }, 0));
		addParam(mpc::intParam("arp_octave", "Arp Octave", 1, 4, 1));
		addParam(mpc::boolParam("arp_latch", "Arp Latch", false));
		addParam(mpc::boolParam("arp_keysync", "Arp KeySync", false));   // the Prophecy's default
		addParam(mpc::choiceParam("arp_step", "Arp Step", { "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T" }, 2));
		addParam(mpc::intParam("arp_gate", "Arp Gate", 1, kp::Arp::kGateStep, 80, "%"));   // 101 = Step
		addParam(mpc::intParam("arp_velocity", "Arp Velocity", 0, kp::Arp::kVelStep, 0));   // 0 = Key, 128 = Step
		addParam(mpc::boolParam("arp_sort", "Arp Sort", true));
		addParam(mpc::intParam("arp_zone_lo", "Arp Zone Lo", 0, 127, 0));
		addParam(mpc::intParam("arp_zone_hi", "Arp Zone Hi", 0, 127, 127));
		addParam(mpc::intParam("arp_bpm", "Arp BPM", 40, 300, 120));
		for (int k = 1; k <= kp::pe::kKnobs; k++)
		{
			const std::string id = "pe_knob" + std::to_string(k), name = "Perf Knob " + std::to_string(k);
			addParam(mpc::intParam(id.c_str(), name.c_str(), 0, 127, 0));
		}
		for (int s = 1; s <= kp::pe::kSets; s++)
		{
			const std::string id = "pe_sw" + std::to_string(s), name = "PE" + std::to_string(s) + " On";
			addParam(mpc::boolParam(id.c_str(), name.c_str(), true));
		}
		addParam(mpc::floatParam("master_tune", "Master Tune", 430.0f, 450.0f, 440.0f, "Hz", 1));
		addParam(mpc::boolParam("dlyrev", "Dly/Rev On", true));
		{
			static const char *const n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
			for (int k = 0; k < 12; k++)
			{
				const std::string id = "us1_" + std::to_string(k), name = std::string("User1 ") + n[k];
				addParam(mpc::intParam(id.c_str(), name.c_str(), -100, 100, 0, "ct"));
			}
		}
		{
			std::vector<std::string> arpc = { "Off", "PBend", "ATouch" }, knobc = { "Off", "PBend+/-", "PBend+", "PBend-", "ATouch" };
			for (int n = 0; n < 96; n++)
			{
				arpc.push_back("CC#" + std::to_string(n));
				knobc.push_back("CC#" + std::to_string(n));
			}
			addParam(mpc::choiceParam("arp_velctrl", "Arp Vel Ctrl", arpc, 0));
			addParam(mpc::choiceParam("arp_gatectrl", "Arp Gate Ctrl", arpc, 0));
			addParam(mpc::intParam("arp_velint", "Arp Vel Int", -99, 99, 99));
			addParam(mpc::intParam("arp_gateint", "Arp Gate Int", -99, 99, 99));
			for (int k = 1; k <= kp::pe::kKnobs; k++)
			{
				const std::string id = "knob" + std::to_string(k) + "_ctrl", name = "Knob" + std::to_string(k) + " Ctrl";
				addParam(mpc::choiceParam(id.c_str(), name.c_str(), knobc, 5 + 18 + k));   // CC#19..23
			}
			addParam(mpc::choiceParam("transpose_pos", "Transp Pos", { "Post-KBD", "Pre-TG" }, 0));
			static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
			for (int k = 0; k < 128; k++)
			{
				const std::string id = "us2_" + std::to_string(k), name = "User2 " + std::string(nn[k % 12]) + std::to_string(k / 12 - 1);
				addParam(mpc::intParam(id.c_str(), name.c_str(), -100, 100, 0, "ct"));
			}
			static const char *const ca[7][2] = { { "ca_wheel1", "Wheel1 Ctrl" }, { "ca_wheel2", "Wheel2 Ctrl" },
				{ "ca_wheel3p", "Wheel3+ Ctrl" }, { "ca_wheel3m", "Wheel3- Ctrl" }, { "ca_ribbonx", "RibbonX Ctrl" },
				{ "ca_ribbonz", "RibbonZ Ctrl" }, { "ca_pedal", "Pedal Ctrl" } };
			static const int ca_def[7] = { 1, 6, 7, 8, 21, 22, 23 };   // what most factory programs use
			for (int k = 0; k < 7; k++)
				addParam(mpc::choiceParam(ca[k][0], ca[k][1], knobc, ca_def[k]));
			addParam(mpc::choiceParam("ca_footsw", "Foot SW", { "Off", "Sustain", "Octave Up", "Octave Down", "Porta SW",
				"Dist SW", "Wah SW", "Delay SW", "Chorus SW", "Reverb SW", "Arp On/Off", "Wheel3 Hold" }, 1));
			addParam(mpc::boolParam("ca_ribxcenter", "RibX Center", false));
			addParam(mpc::intParam("peed_set", "PE Ed Set", 1, kp::pe::kSets, 1));
			addParam(mpc::intParam("peed_knob", "PE Ed Knob", 1, kp::pe::kKnobs, 1));
			std::vector<std::string> pen = { "Off" };   // PE numbers by the parameter they edit
			for (int n = 1; n < 256; n++)
			{
				const int i = kp::prm::index_of(kp::prm::kPeAssign[n]);
				pen.push_back(i >= 0 ? kp::prm::kDefs[i].name : "#" + std::to_string(n));
			}
			addParam(mpc::choiceParam("pe_param", "PE Param", pen, 0));
			addParam(mpc::intParam("pe_left", "PE Left", 0, 100, 0, "%"));
			addParam(mpc::intParam("pe_right", "PE Right", 0, 100, 100, "%"));
			addParam(mpc::choiceParam("pe_curve", "PE Curve", { "Linear", "Exp", "Log" }, 0));
			addParam(mpc::intParam("pated_step", "Pat Ed Step", 1, kp::Arp::kSteps, 1));
			addParam(mpc::intParam("pat_tone", "Pat Tone", 1, kp::Arp::kLoop, 1));        // 13 = Loop
			addParam(mpc::intParam("pat_velocity", "Pat Velocity", 1, 127, 100));
			addParam(mpc::intParam("pat_gate", "Pat Gate", 0, 100, 80, "%"));            // 0 = Off (rest)
			addParam(mpc::intParam("pat_offset", "Pat Offset", -49, 49, 0, "%"));
			addParam(mpc::choiceParam("arp_type", "Arp Type", { "As Played", "Fill", "Running Up", "Up&Down" }, 2));
			addParam(mpc::choiceParam("arp_octalt", "Arp Oct Alt", { "Up", "Down", "Up&Down" }, 0));
			for (int k = 1; k <= 3; k++)   // what the MPC sends (pads, Q-Links, controllers): newest first
			{
				const std::string id = "midi_in" + std::to_string(k), name = "MIDI In " + std::to_string(k);
				mpc::Param m = mpc::intParam(id.c_str(), name.c_str(), 0, 999, 0);
				m.automatable = true;   // (see Engine Load)
				addParam(m);
			}
			{
				mpc::Param m = mpc::boolParam("midi_log", "MIDI Log", false);
				m.automatable = true;   // (see Engine Load)
				addParam(m);
			}
			addParam(mpc::boolParam("idle_sleep", "Idle Sleep", true));
			addParam(mpc::intParam("voices", "Voices", 1, kp::PolySynth::kMaxVoices, 1));
		}
#ifdef __linux__
		// the device's processor, for tuning the build (Content/device_info.txt, written at load)
		if (std::FILE *ci = std::fopen("/proc/cpuinfo", "r"))
		{
			if (std::FILE *o = std::fopen((std::filesystem::path(mpc::contentFolder()) / "device_info.txt").string().c_str(), "w"))
			{
				char line[512];
				while (std::fgets(line, sizeof line, ci))
					std::fputs(line, o);
				if (std::FILE *v = std::fopen("/proc/version", "r"))
				{
					if (std::fgets(line, sizeof line, v)) std::fputs(line, o);
					std::fclose(v);
				}
				std::fclose(o);
			}
			std::fclose(ci);
		}
#endif
		log_path_ = (std::filesystem::path(mpc::contentFolder()) / "midi_log.txt").string();
		log_thread_ = std::thread([this] { log_loop(); });
		if (numParams() != kNumParams)   // the index constants above must match the declarations
			std::fprintf(stderr, "KP Sim Prophetic: %d parameters declared, %d expected\n", numParams(), kNumParams);

		std::string err;
		ok_ = synth_.load(mpc::contentFolder(), err);
		if (ok_ && synth_.booted())   // no snapshot in Content: booted the firmware natively, keep it for next time
			synth_.save_snapshot(mpc::contentFolder());
		if (ok_)   // your own SysEx libraries: Content/User/*.syx (programs after the factory ones)
		{
			std::string report;
			synth_.load_user_sysex((std::filesystem::path(mpc::contentFolder()) / "User").string(), &report);
			synth_.load_voices_async();   // the other voices load in the background (Voices takes them as they arrive)
			if (!report.empty())
				std::fprintf(stderr, "KP Sim Prophetic: User SysEx\n%s", report.c_str());
		}
		if (!ok_)
			std::fprintf(stderr, "KP Sim Prophetic: cannot load %s: %s\n", mpc::contentFolder().c_str(), err.c_str());
		if (ok_ && !synth_.has_model_defaults())
			std::fprintf(stderr, "KP Sim Prophetic: no ic12.bin in Content/: OSC Set changes keep the old model settings\n");
		for (int i = 0; i < kp::Arp::kPatterns; i++)
			if (const std::uint8_t *b = synth_.arp_pattern(i))
			{
				std::memcpy(pattern_bytes_[size_t(i)].data(), b, 128);
				arp_patterns_[size_t(i)] = kp::Arp::PatternData::from_bytes(b);
			}
		if (ok_ && !synth_.has_perf())
			std::fprintf(stderr, "KP Sim Prophetic: no usable ic12.bin in Content/: the Performance Editor knobs do nothing\n");
		preset_raw_.resize(size_t(synth_.program_count()));
		preset_perf_.resize(size_t(synth_.program_count()));
		preset_assign_.resize(size_t(synth_.program_count()));
		for (int p = 0; p < synth_.program_count(); p++)
		{
			preset_perf_[p] = synth_.program_perf(p);
			for (int k = 0; k < kp::pe::kKnobs; k++)   // the knobs show where they agree with the program
				if (const int x = synth_.program_knob_match(p, k); x >= 0)
					preset_perf_[p].knob[k] = std::uint8_t(x);
			std::uint8_t dump[kp::pe::kDumpSize];
			if (synth_.program_dump(p, dump))
				std::memcpy(preset_assign_[p].data(), &dump[435], 9);
			else
				preset_assign_[p] = { 1, 6, 7, 8, 21, 22, 23, 1, 0 };
			preset_raw_[p] = synth_.program_params(p);
			std::vector<float> v(kPresetParams, 0.0f);
			v[kVolume] = 12.0f;
			for (int i = 0; i < kp::prm::kCount; i++)
				v[kFirstProgParam + i] = plain_of(i, preset_raw_[p][i]);
			v[kPorta] = synth_.program_porta(p) ? 1.0f : 0.0f;
			const int oscset = preset_raw_[p][osc_set_index()];
			v[kOsc1Model] = float(kp::prm::model_of(oscset, 1));
			v[kOsc2Model] = float(kp::prm::model_of(oscset, 2));
			addProgram(synth_.program_name(p).c_str(), v);
		}
	}

	// after a state or preset load (audio thread, before the next block): keep the loaded arpeggiator values
	// The host resets the plugin on a state load and when it restarts the audio stream; the MPC
	// does the latter on Stop (measured: the MIDI Log's FF markers, no MIDI message arrives). Both
	// mean "silence": the audio thread stops the arpeggiator, releases every key, sends the
	// Prophecy's All Notes Off and All Sound Off, and fades the output out (panic()).
	void reset() override
	{
		arp_pattern_ = -1;
		pattern_load_ = -1;
		panic_.store(true);
		log_marker(0xff, 0, 0);   // MIDI Log: the host reset the plugin (Stop, state or preset load)
	}

	void prepare(double sampleRate, int maxBlockSize) override
	{
		rs_.configure(kp::Synth::kSampleRate, sampleRate);
		scratchL_.assign(size_t(maxBlockSize) * 2 + 64, 0.0f);
		scratchR_.assign(size_t(maxBlockSize) * 2 + 64, 0.0f);
	}

	void parameterChanged(int index, float plain) override
	{
		if (index != kVolume)
			return;
		if (currentProgram() != ui_program_)
		{
			ui_program_ = currentProgram();
			revert_volume_.store(true);
		}
		else
			user_volume_.store(plain);
	}

	bool formatValue(int index, float plain, std::string &out) override
	{
		if (index >= kMidiIn1 && index < kMidiIn1 + 3)
		{
			const std::uint32_t m = midi_log_[size_t(index - kMidiIn1)].load(std::memory_order_relaxed);
			const int n = int(m >> 24);
			char b[16] = "-";
			if (n == 1) std::snprintf(b, sizeof b, "%02X", unsigned(m & 0xff));
			else if (n == 2) std::snprintf(b, sizeof b, "%02X %02X", unsigned(m & 0xff), unsigned((m >> 8) & 0xff));
			else if (n == 3) std::snprintf(b, sizeof b, "%02X %02X %02X", unsigned(m & 0xff), unsigned((m >> 8) & 0xff), unsigned((m >> 16) & 0xff));
			out = b;
			return true;
		}
		if (index == kLoad)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%.0f/%.0f%%", avg_load_, worst_load_);   // average / peak
			out = b;
			return true;
		}
		const int i = index - kFirstProgParam;
		if (i >= 0 && i < kp::prm::kCount && kp::prm::kDefs[i].fmt != kp::prm::Fmt::Choice)
		{
			out = kp::prm::text(kp::prm::kDefs[i], int(std::lround(plain)));
			return true;
		}
		if (index == kArpVelocity && (std::lround(plain) == kp::Arp::kVelKey || std::lround(plain) == kp::Arp::kVelStep))
		{
			out = std::lround(plain) == kp::Arp::kVelKey ? "Key" : "Step";
			return true;
		}
		if (index == kPatTone && std::lround(plain) == kp::Arp::kLoop)
		{
			out = "Loop";
			return true;
		}
		if (index == kPatGate && std::lround(plain) == 0)
		{
			out = "Off";
			return true;
		}
		if (index == kArpGate && std::lround(plain) == kp::Arp::kGateStep)
		{
			out = "Step";
			return true;
		}
		if (index == kArpZoneLo || index == kArpZoneHi)
		{
			static const char *const n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
			const int v = int(std::lround(plain));
			char b[16];
			std::snprintf(b, sizeof b, "%s%d", n[v % 12], v / 12 - 1);
			out = b;
			return true;
		}
		if (index == kTranspose)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%+d", int(std::lround(plain)));
			out = b;
			return true;
		}
		return false;
	}

	// version 2: version, program the edits are relative to, the loaded program's Performance Editor
	// Define (80 bytes), the 10 arpeggio patterns (10 x 128)
	static constexpr size_t kExtraV2 = 8 + 80 + 10 * 128;
	void saveExtraState(std::vector<uint8_t> &out) override
	{
		const int pending = restore_program_.load();
		const int32_t v[2] = { 2, pending >= 0 ? pending : applied_program_ };
		out.resize(kExtraV2);
		std::memcpy(out.data(), v, sizeof v);
		if (staged_valid_.load())   // a loaded state not yet applied by the audio thread: save that one
		{
			std::memcpy(out.data() + 8, staged_extra_.data(), staged_extra_.size());
			return;
		}
		for (int k = 0; k < 80; k++)
			out[8 + size_t(k)] = std::uint8_t(synth_.program_extra(444 + k));
		for (int i = 0; i < 10; i++)
			std::memcpy(&out[88 + 128 * size_t(i)], pattern_bytes_[size_t(i)].data(), 128);
	}

	void loadExtraState(const uint8_t *data, size_t size) override
	{
		int32_t v[2] = {};
		if (size >= sizeof v)
		{
			std::memcpy(v, data, sizeof v);
			if (v[0] == 2 && size >= kExtraV2)
			{
				std::memcpy(staged_extra_.data(), data + 8, kExtraV2 - 8);
				staged_valid_.store(true);
			}
			if ((v[0] == 1 || v[0] == 2) && v[1] >= 0)
				restore_program_.store(v[1]);
		}
	}

	void process(mpc::ProcessContext &ctx) override
	{
		if (!ok_)
			return;
		const auto t_start = std::chrono::steady_clock::now();   // Engine Load: this whole block
		// Voices: as many as have loaded (the rest join as they arrive); the voices added are
		// brought up to date after this block's edits (resync_voices), the others play on
		const int voices = getInt(kVoices), voices_before = synth_.voices();
		if (voices != voices_before)
			synth_.set_voices(voices);
		const int first_added = synth_.voices() > voices_before ? voices_before : 0;
		log_on_.store(getBool(kMidiLog), std::memory_order_relaxed);
		synth_.set_idle_sleep(getBool(kIdleSleep));
		log_transport(ctx.transport);
		if (panic_.exchange(false))
			panic();
		if (ctx.sampleRate > 0)
			log_rate_.store(ctx.sampleRate);
		const int restore = restore_program_.exchange(-1);
		if (restore >= 0)
		{
			host_program_ = currentProgram();
			load_program(restore, true);            // project: program, then its edited values
			if (staged_valid_.exchange(false))      // and its Define edits and arpeggio patterns
			{
				for (int k = 0; k < 80; k++)
					synth_.set_program_extra(444 + k, staged_extra_[size_t(k)]);
				for (int i = 0; i < 10; i++)
				{
					std::memcpy(pattern_bytes_[size_t(i)].data(), &staged_extra_[80 + 128 * size_t(i)], 128);
					arp_patterns_[size_t(i)] = kp::Arp::PatternData::from_bytes(pattern_bytes_[size_t(i)].data());
				}
				pe_refresh_ = pat_refresh_ = true;
			}
		}
		else if (currentProgram() != host_program_)
		{
			host_program_ = currentProgram();       // preset chosen on the host
			load_program(host_program_, false);     // the program's own values
		}
		if (revert_volume_.exchange(false))
			set(kVolume, user_volume_.load());      // the master volume is not part of a program
		sync_edits();
		if (first_added > 0)
			resync_voices(first_added);
		update_arp();
		for (int e = 0; e < ctx.numMidiEvents; ++e)
			handle(ctx.midi[e]);
		{
			const auto &t = ctx.transport;
			const double tempo = t.valid && t.tempoValid ? t.bpm : 0.0;
			const double ppq = t.valid && t.playing && t.ppqValid ? t.ppqPos : -1.0;
			arp_.run(arp_set_, ctx.numFrames, ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0, tempo, ppq, [this](const kp::Arp::Out &o) {
				if (o.on) synth_.note_on(o.note, o.velocity);
				else synth_.note_off(o.note, o.velocity);
			});
		}

		const int n = ctx.numFrames;
		frames_ += std::uint64_t(n > 0 ? n : 0);   // MIDI Log time base
		int need = rs_.needed(n);
		while (need > 0)
		{
			const int chunk = need < int(scratchL_.size()) ? need : int(scratchL_.size());
			synth_.render(scratchL_.data(), scratchR_.data(), chunk);
			rs_.push(scratchL_.data(), scratchR_.data(), chunk);
			need -= chunk;
		}
		float *L = ctx.outputs[0];
		float *R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
		rs_.pull(L, R ? R : scratchR_.data(), n);
		// Volume, then a soft knee above 0.85 so overloads round off instead of clipping hard
		// (the same output stage as the Windows test app).
		const float g = std::pow(10.0f, get(kVolume) / 20.0f);
		auto knee = [](float v) {
			const float a = std::fabs(v);
			return a > 0.85f ? std::copysign(0.85f + 0.15f * std::tanh((a - 0.85f) / 0.15f), v) : v;
		};
		if (gate_ == 1.0f && gate_target_ == 1.0f)
			for (int i = 0; i < n; i++)
			{
				L[i] = knee(L[i] * g);
				if (R) R[i] = knee(R[i] * g);
			}
		else
		{
			// Stop fade (down over kFadeMs, back up over 2 ms at the next note)
			const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
			const float down = float(1000.0 / (kFadeMs * sr)), up = float(1000.0 / (2.0 * sr));
			for (int i = 0; i < n; i++)
			{
				gate_ = gate_target_ > gate_ ? std::min(gate_target_, gate_ + up) : std::max(gate_target_, gate_ - down);
				L[i] = knee(L[i] * g * gate_);
				if (R) R[i] = knee(R[i] * g * gate_);
			}
			// faded out: what the engine still plays is not heard, so it may sleep (1 s from here)
			synth_.set_muted(gate_target_ == 0.0f && gate_ == 0.0f);
		}
		// Engine Load: time spent in this block / the block's real-time length (one core), as a
		// running average (about 1 s) and a decaying peak; the value changes so the screen redraws
		if (n > 0 && ctx.sampleRate > 0)
		{
			const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
			const double pct = 100.0 * used * ctx.sampleRate / n;
			avg_load_ += (pct - avg_load_) * std::min(1.0, n / ctx.sampleRate);
			worst_load_ = pct > worst_load_ ? pct : worst_load_ - (worst_load_ - avg_load_) * std::min(1.0, n / (3.0 * ctx.sampleRate));
			set(kLoad, float(avg_load_ < 100.0 ? avg_load_ : 100.0));
			// MIDI Log: one load line per second (exact over that second, the log thread writes it)
			acc_used_ += used;
			acc_real_ += n / ctx.sampleRate;
			acc_peak_ = pct > acc_peak_ ? pct : acc_peak_;
			if (synth_.sleeping())
				acc_asleep_ += n / ctx.sampleRate;
			if (acc_real_ >= 1.0)
			{
				pub_avg_.store(100.0 * acc_used_ / acc_real_, std::memory_order_relaxed);
				pub_peak_.store(acc_peak_, std::memory_order_relaxed);
				pub_asleep_.store(100.0 * acc_asleep_ / acc_real_, std::memory_order_relaxed);
#if defined(__linux__)
				pub_cpu_.store(sched_getcpu(), std::memory_order_relaxed);   // which core runs the audio
#endif
				pub_time_.store(double(frames_) / ctx.sampleRate, std::memory_order_relaxed);
				pub_seq_.fetch_add(1, std::memory_order_release);
				acc_used_ = acc_real_ = acc_peak_ = acc_asleep_ = 0;
			}
		}
	}

private:
	static int osc_set_index()
	{
		static const int i = kp::prm::index_of(kp::prm::kOscSetId);
		return i;
	}
	// plugin plain value <-> Prophecy raw value
	static float plain_of(int i, int raw)
	{
		const kp::prm::Def &d = kp::prm::kDefs[i];
		raw = raw < d.lo ? d.lo : (raw > d.hi ? d.hi : raw);
		return float(d.fmt == kp::prm::Fmt::Choice ? raw - d.base : raw);
	}
	int raw_of(int i) const
	{
		const kp::prm::Def &d = kp::prm::kDefs[i];
		const int v = int(std::lround(get(kFirstProgParam + i)));
		return d.fmt == kp::prm::Fmt::Choice ? v + d.base : v;
	}

	void load_program(int p, bool keep_values)
	{
		if (p < 0 || p >= synth_.program_count())
			return;
		applied_program_ = p;
		restoring_ = keep_values;
		synth_.program(p);
		synth_.midi_cc(1, mod_);
		for (int i = 0; i < kp::prm::kCount; i++)
		{
			baseline_[i] = preset_raw_[p][i];
			if (!keep_values)
				set(kFirstProgParam + i, plain_of(i, baseline_[i]));
		}
		porta_ = synth_.program_porta(p);
		if (!keep_values)
			set(kPorta, porta_ ? 1.0f : 0.0f);
		// Performance Editor: the engine now has the program's set switches and stored knob positions.
		// The knobs show, and the engine takes as its knob positions without applying anything (the
		// sound stays the program's), the positions that agree best with the program's own values
		// (Synth::program_knob_match): the stored ones need not (the V55 applies a knob's sets only
		// when it moves), so a first move from them would jump. A restored project then moves them
		// to its saved values, again without applying them (sync_edits).
		const kp::pe::Program &pe = preset_perf_[p];
		for (int k = 0; k < kp::pe::kKnobs; k++)
		{
			knob_pos_[k] = pe.valid ? pe.knob[k] : 0;
			synth_.set_perf_position(k, knob_pos_[k]);
			if (!keep_values)
				set(kKnob1 + k, float(knob_pos_[k]));
		}
		for (int s = 0; s < kp::pe::kSets; s++)
		{
			pe_on_[s] = pe.valid && ((pe.sw >> s) & 1);
			if (!keep_values)
				set(kPeSw1 + s, pe_on_[s] ? 1.0f : 0.0f);
		}
		for (int k = 0; k < 9; k++)
		{
			ca_shown_[k] = preset_assign_[p][k];
			if (!keep_values)
				set(kCtlAssign1 + k, float(ca_shown_[k]));
		}
		pe_refresh_ = true;   // the Define editor shows the new program's slot
		update_models();
	}

	// A Performance Editor knob moved (parameter, Q-Link or MIDI CC19..23): the engine applies the
	// knob's PE sets, and the edited parameters show their new values.
	void knob_moved(int k, int v)
	{
		knob_pos_[k] = v;
		kp::pe::Edit e[kp::pe::kSets];
		const int n = synth_.perf_knob(k, v, e);
		for (int i = 0; i < n; i++)
		{
			baseline_[e[i].index] = e[i].value;
			set(kFirstProgParam + e[i].index, plain_of(e[i].index, e[i].value));
		}
	}

	// NRPN arpeggiator / PE switch changes (Parameter Guide 2-1), as parameter changes.
	void apply_nrpn(const kp::Synth::Nrpn &n)
	{
		auto sw = [&](int index) {
			const bool on = n.step != 0 ? n.step > 0 : n.value >= 0x40;
			set(index, on ? 1.0f : 0.0f);
		};
		auto step = [&](int index, int lo, int hi, int value) {
			const int v = n.step != 0 ? getInt(index) + n.step : value;
			if (v >= lo && v <= hi)
				set(index, float(v));
		};
		switch (n.number)
		{
		case 1: step(kArpPattern, 0, 9, n.value); break;   // 0..4 preset patterns, 5..9 PAT1-5
		case 2: sw(kArpOn); break;
		case 3: step(kArpOctave, 1, 4, n.value + 1); break;
		case 4: sw(kArpLatch); break;
		case 5: sw(kArpKeySync); break;
		case 6: case 7: case 8: case 9:
		{
			const int s = n.number - 6;   // the engine has switched it already
			pe_on_[s] = synth_.pe_switch_on(s);
			set(kPeSw1 + s, pe_on_[s] ? 1.0f : 0.0f);
			break;
		}
		default: break;
		}
	}

	void update_models()
	{
		const int oscset = baseline_[osc_set_index()];
		set(kOsc1Model, float(kp::prm::model_of(oscset, 1)));
		set(kOsc2Model, float(kp::prm::model_of(oscset, 2)));
	}

	// Send every parameter that differs from what the engine holds (cheap: one compare each).
	void sync_edits()
	{
		const int os = osc_set_index();
		const int oscset = raw_of(os);
		if (oscset != baseline_[os])
		{
			synth_.set_param(os, oscset);   // reloads the program; changed slots get the model defaults
			baseline_[os] = oscset;
			for (int i = 0; i < kp::prm::kCount; i++)
				if (kp::prm::kDefs[i].model != kp::prm::kNone && synth_.param_active(i))
				{
					baseline_[i] = synth_.param(i);
					// A live OSC Set change shows the defaults; a restored project keeps its values (sent below).
					if (!restoring_)
						set(kFirstProgParam + i, plain_of(i, baseline_[i]));
				}
			update_models();
		}
		for (int i = 0; i < kp::prm::kCount; i++)
		{
			if (i == os)
				continue;
			const int v = raw_of(i);
			if (v == baseline_[i])
				continue;
			baseline_[i] = v;
			if (synth_.param_active(i))
				synth_.set_param(i, v);   // inactive model parameters have no effect (as on the Prophecy)
		}
		for (int k = 0; k < 9; k++)   // Controller Assignment (per program)
		{
			const int v = getInt(kCtlAssign1 + k);
			if (v != ca_shown_[k])
			{
				ca_shown_[k] = v;
				synth_.set_program_extra(kp::Synth::kExtraFirst + k, v);
			}
		}
		sync_pe_define();
		for (int s = 0; s < kp::pe::kSets; s++)
		{
			const bool on = getBool(kPeSw1 + s);
			if (on != pe_on_[s])
			{
				pe_on_[s] = on;
				synth_.pe_switch(s, on);
			}
		}
		for (int k = 0; k < kp::pe::kKnobs; k++)
		{
			const int v = getInt(kKnob1 + k);
			if (v == knob_pos_[k])
				continue;
			if (restoring_)
			{
				knob_pos_[k] = v;
				synth_.set_perf_position(k, v);   // the saved parameter values already include its effect
			}
			else
				knob_moved(k, v);
		}
		restoring_ = false;
		const bool porta = getBool(kPorta);
		if (porta != porta_)
		{
			porta_ = porta;
			synth_.midi_cc(65, porta ? 127 : 0);
		}
		const int tr = getInt(kTranspose);
		if (tr != transpose_)
		{
			transpose_ = tr;
			synth_.set_transpose(tr);
		}
		// global settings: sent once at start (the engine's snapshot has the Prophecy's own), then on change
		const int mt = int(std::lround((get(kMasterTune) - 440.0f) * 10.0f));
		if (mt != master_tune_)
		{
			master_tune_ = mt;
			synth_.set_master_tune(mt);
		}
		const int dr = getBool(kDlyRev) ? 1 : 0;
		if (dr != dlyrev_)
		{
			dlyrev_ = dr;
			synth_.set_delay_reverb(dr != 0);
		}
		for (int k = 0; k < 12; k++)
		{
			const int c = getInt(kUserScale1 + k);
			if (c != user_scale_[k])
			{
				user_scale_[k] = c;
				synth_.set_user_scale(1, k, c);
			}
		}
		for (int k = 0; k < 128; k++)
		{
			const int c = getInt(kUserScale2 + k);
			if (c != user_scale2_[k])
			{
				user_scale2_[k] = c;
				synth_.set_user_scale(2, k, c);
			}
		}
		const int tp = getInt(kTransposePos);
		if (tp != transpose_pos_)
		{
			transpose_pos_ = tp;
			synth_.set_transpose_position(tp);
		}
		for (int k = 0; k < kp::pe::kKnobs; k++)
		{
			const int c = getInt(kKnobCtl1 + k);
			if (c != knob_ctl_[k])
			{
				knob_ctl_[k] = c;
				synth_.set_knob_assign(k, c);   // which MIDI message turns the knob (and what it sends)
			}
		}
		// display-only model parameters follow OSC Set
		const int m1 = kp::prm::model_of(baseline_[os], 1), m2 = kp::prm::model_of(baseline_[os], 2);
		if (getInt(kOsc1Model) != m1) set(kOsc1Model, float(m1));
		if (getInt(kOsc2Model) != m2) set(kOsc2Model, float(m2));
	}

	// Performance Editor Define: the selected set / knob's PE number, left, right, curve; edits go to
	// the loaded program (from the next knob move, as on the Prophecy)
	void sync_pe_define()
	{
		const int es = getInt(kPeEdSet) - 1, ek = getInt(kPeEdKnob) - 1;
		const int base = 444 + 20 * es + 4 * ek;
		if (pe_refresh_ || es != pe_sel_[0] || ek != pe_sel_[1])
		{
			pe_refresh_ = false;
			pe_sel_[0] = es;
			pe_sel_[1] = ek;
			for (int f = 0; f < 4; f++)
			{
				pe_shown_[f] = synth_.program_extra(base + f);
				set(kPeParam + f, float(pe_shown_[f]));
			}
			return;
		}
		for (int f = 0; f < 4; f++)
		{
			const int v = getInt(kPeParam + f);
			if (v != pe_shown_[f])
			{
				pe_shown_[f] = v;
				synth_.set_program_extra(base + f, v);
			}
		}
	}

	// Arpeggio Pattern Define for the selected pattern: the edited step (tone, velocity, gate, offset)
	// and the pattern's type / octave alternation; the pattern also keeps the Step / Sort / zone /
	// Velocity / Gate / intensities set while it is selected (each pattern has its own, as on the
	// Prophecy)
	void sync_pattern_define(int pattern)
	{
		auto &b = pattern_bytes_[size_t(pattern)];
		const int st = getInt(kPatEdStep) - 1, at = 32 + 4 * st;
		if (pat_refresh_ || pattern != pat_sel_[0] || st != pat_sel_[1])
		{
			pat_refresh_ = false;
			pat_sel_[0] = pattern;
			pat_sel_[1] = st;
			pat_shown_ = { b[at + 1] < 1 ? 1 : (b[at + 1] > kp::Arp::kLoop ? kp::Arp::kLoop : b[at + 1]), b[at + 2] < 1 ? 1 : (b[at + 2] > 127 ? 127 : b[at + 2]),
			               b[at + 3] > 100 ? 100 : b[at + 3], int(std::int8_t(b[at])), b[27] > 3 ? 3 : b[27], b[28] > 2 ? 2 : b[28] };
			for (int f = 0; f < 6; f++)
				set(kPatTone + f, float(pat_shown_[size_t(f)]));
			return;
		}
		bool changed = false;
		for (int f = 0; f < 6; f++)
		{
			const int v = getInt(kPatTone + f);
			if (v == pat_shown_[size_t(f)])
				continue;
			pat_shown_[size_t(f)] = v;
			changed = true;
			switch (f)
			{
			case 0: b[at + 1] = std::uint8_t(v); break;
			case 1: b[at + 2] = std::uint8_t(v); break;
			case 2: b[at + 3] = std::uint8_t(v); break;
			case 3: b[at] = std::uint8_t(std::int8_t(v)); break;
			case 4: b[27] = std::uint8_t(v); break;
			default: b[28] = std::uint8_t(v); break;
			}
		}
		// the pattern's own settings follow the arpeggiator parameters while it is selected
		const int vel = getInt(kArpVelocity);
		const std::uint8_t base[8] = { std::uint8_t(getInt(kArpStep)), std::uint8_t(getBool(kArpSort)), std::uint8_t(getInt(kArpZoneLo)),
		                               std::uint8_t(getInt(kArpZoneHi)),
		                               std::uint8_t(vel == kp::Arp::kVelKey ? 128 : (vel == kp::Arp::kVelStep ? 129 : vel)),
		                               std::uint8_t(std::int8_t(getInt(kArpVelInt))), std::uint8_t(getInt(kArpGate)),
		                               std::uint8_t(std::int8_t(getInt(kArpGateInt))) };
		static const int pos[8] = { 17, 18, 19, 20, 21, 23, 24, 26 };
		for (int k = 0; k < 8; k++)
			if (b[size_t(pos[k])] != base[k])
			{
				b[size_t(pos[k])] = base[k];
				changed = true;
			}
		if (changed)
			arp_patterns_[size_t(pattern)] = kp::Arp::PatternData::from_bytes(b.data());
	}

	// Silence (host reset, see reset()): All Sound Off clears the voices and the delay / reverb
	// lines within about 50 ms on most programs, but not everything (factory program 000 keeps
	// decaying for about 11 s, the hardware's own behaviour), so the output also fades out over
	// kFadeMs and stays muted until the next note on, and once faded the engine counts as silent
	// for idle sleep (Synth::set_muted): it sleeps 1 s after the Stop. The next note wakes it where
	// it stopped, the way idle sleep always resumes.
	// Voices added (first.. on): they take the current state, as for a project load, while the
	// voices already playing are left alone (only_from): the program with its edits (sync_edits
	// sends what differs from the program), the Performance Editor Define edits, the global
	// settings (the caches forget what was sent, so the sync re-sends them), then voice 0's
	// controllers (wheels, pedals, bend, aftertouch, the PE knobs' CCs). The caches end as before:
	// the playing voices already hold every value the sync sends.
	void resync_voices(int first)
	{
		if (applied_program_ < 0)
			return;
		std::array<int, 80> define{};
		for (int k = 0; k < 80; k++)   // voice 0's
			define[size_t(k)] = synth_.program_extra(444 + k);
		synth_.only_from(first);
		load_program(applied_program_, true);
		for (int k = 0; k < 80; k++)
			synth_.set_program_extra(444 + k, define[size_t(k)]);
		master_tune_ = -1000;
		dlyrev_ = -1;
		transpose_ = 1000;
		transpose_pos_ = -1;
		for (auto &u : user_scale_)
			u = -1000;
		user_scale2_ = make_unsent();
		for (auto &c : knob_ctl_)
			c = -1;
		sync_edits();
		synth_.match_controllers(first);
		synth_.only_from(0);
	}

	static constexpr float kFadeMs = 30.0f;
	void panic()
	{
		stop_arp();
		for (int n = 0; n < 128; n++)
			if (held_[n]) { synth_.note_off(n); held_[n] = false; }
		synth_.midi_cc(123, 0);
		synth_.midi_cc(120, 0);
		gate_target_ = 0.0f;
	}

	void stop_arp()
	{
		arp_.stop([this](const kp::Arp::Out &o) { if (!o.on) synth_.note_off(o.note, o.velocity); });
	}

	void update_arp()
	{
		kp::Arp::Settings s;
		s.on = getBool(kArpOn);
		s.pattern = getInt(kArpPattern);
		// Selecting a pattern loads its own settings, as on the Prophecy. Applied one block later so
		// that a project or preset load (reset() runs after it) keeps the values it restored.
		if (pattern_load_ >= 0 && pattern_load_ == s.pattern)
		{
			const kp::Arp::PatternData &pd = arp_patterns_[size_t(s.pattern)];
			if (pd.valid)
			{
				set(kArpStep, float(pd.step));
				set(kArpSort, pd.sort ? 1.0f : 0.0f);
				set(kArpZoneLo, float(pd.zone_lo));
				set(kArpZoneHi, float(pd.zone_hi));
				set(kArpVelocity, float(pd.velocity));
				set(kArpGate, float(pd.gate));
				set(kArpVelInt, float(pd.vel_int));
				set(kArpGateInt, float(pd.gate_int));
			}
		}
		const bool loaded = pattern_load_ >= 0;
		pattern_load_ = -1;
		if (s.pattern != arp_pattern_)
		{
			if (arp_pattern_ >= 0)
				pattern_load_ = s.pattern;   // a change (not the first look after a load)
			arp_pattern_ = s.pattern;
		}
		else if (!loaded && pattern_load_ < 0)
			sync_pattern_define(s.pattern);
		s.user = s.pattern >= kp::Arp::kPat1 ? &arp_patterns_[size_t(s.pattern)] : nullptr;
		s.octaves = getInt(kArpOctave);
		s.latch = getBool(kArpLatch);
		s.key_sync = getBool(kArpKeySync);
		s.step = getInt(kArpStep);
		s.gate = getInt(kArpGate);
		s.velocity = getInt(kArpVelocity);
		s.sort = getBool(kArpSort);
		s.zone_lo = getInt(kArpZoneLo);
		s.zone_hi = getInt(kArpZoneHi);
		s.bpm = getInt(kArpBpm);
		s.vel_int = getInt(kArpVelInt);
		s.gate_int = getInt(kArpGateInt);
		s.vel_ctrl = arp_controller(getInt(kArpVelCtrl));
		s.gate_ctrl = arp_controller(getInt(kArpGateCtrl));
		if ((arp_set_.on && !s.on) || (arp_set_.latch && !s.latch))
			stop_arp();   // switched off, or latch released: the arpeggio stops
		arp_set_ = s;
	}

	// current value of an Arpeggiator Controller Assign choice (0 Off, 1 PBend, 2 ATouch, 3 + n CC#n)
	int arp_controller(int c) const
	{
		if (c <= 0) return 0;
		if (c == 1) return synth_.controller_value(0x0e);
		if (c == 2) return synth_.controller_value(0x0f);
		return synth_.controller_value(0x10 + c - 3);
	}

	// knobs a MIDI message turned: their parameters and the edits they made follow
	void apply_moves(const kp::Synth::KnobMoves &m)
	{
		for (int i = 0; i < m.count; i++)
		{
			const auto &mv = m.m[i];
			knob_pos_[mv.knob] = mv.value;
			set(kKnob1 + mv.knob, float(mv.value));
			for (int j = 0; j < mv.n; j++)
			{
				baseline_[mv.e[j].index] = mv.e[j].value;
				set(kFirstProgParam + mv.e[j].index, plain_of(mv.e[j].index, mv.e[j].value));
			}
		}
	}

	// MIDI Log markers for host events (status bytes the MIDI stream never carries to a plugin):
	// FA transport start, FC stop, F2 locate (song position in 16ths, 14 bits), FF reset
	void log_marker(int status, int d1, int d2)
	{
		if (!log_on_.load(std::memory_order_relaxed))
			return;
		const std::uint32_t h = log_head_.load(std::memory_order_relaxed);
		log_msg_[h & (kLogRing - 1)] = std::uint32_t(status) | (std::uint32_t(d1 & 0x7f) << 8) | (std::uint32_t(d2 & 0x7f) << 16) | (3u << 24);
		log_frame_[h & (kLogRing - 1)] = std::uint32_t(frames_);
		log_head_.store(h + 1, std::memory_order_release);
	}

	void log_transport(const mpc::Transport &t)
	{
		const bool playing = t.valid && t.playing;
		const double ppq = t.valid && t.ppqValid ? t.ppqPos : -1.0;
		if (playing != log_playing_)
			log_marker(playing ? 0xfa : 0xfc, 0, 0);
		else if (!playing && ppq != log_ppq_ && ppq >= 0)
		{
			const int sixteenths = int(ppq * 4.0) & 0x3fff;
			log_marker(0xf2, sixteenths & 0x7f, sixteenths >> 7);
		}
		log_playing_ = playing;
		log_ppq_ = ppq;
	}

	void log_midi(const mpc::MidiEvent &ev)
	{
		if (ev.data[0] >= 0xf8)   // clock, active sensing...: not shown
			return;
		const std::uint32_t m = std::uint32_t(ev.data[0]) | (std::uint32_t(ev.data[1]) << 8) | (std::uint32_t(ev.data[2]) << 16) |
		                        (std::uint32_t(ev.size < 1 ? 1 : (ev.size > 3 ? 3 : ev.size)) << 24);
		if (log_on_.load(std::memory_order_relaxed))   // for the log thread: a lock-free single-producer ring
		{
			const std::uint32_t h = log_head_.load(std::memory_order_relaxed);
			log_msg_[h & (kLogRing - 1)] = m;
			log_frame_[h & (kLogRing - 1)] = std::uint32_t(frames_ + std::uint64_t(ev.sampleOffset));
			log_head_.store(h + 1, std::memory_order_release);
		}
		for (int k = 2; k > 0; k--)
			midi_log_[size_t(k)].store(midi_log_[size_t(k - 1)].load(std::memory_order_relaxed), std::memory_order_relaxed);
		midi_log_[0].store(m, std::memory_order_relaxed);
		midi_count_ = (midi_count_ + 1) % 1000;
		for (int k = 0; k < 3; k++)   // a changing value so the host redraws the text
			set(kMidiIn1 + k, float((midi_count_ + k) % 1000));
	}

	// MIDI Log: a background thread (never the audio thread) appends the logged messages to
	// Content/midi_log.txt every 200 ms while the switch is on
	// Every second while the switch is on it also writes the engine's load over that second:
	//   <seconds> LOAD avg <% of one core> peak <% of the worst block> asleep <% idle sleep> cpu <core>
	void log_loop()
	{
		std::uint32_t tail = 0, seq = pub_seq_.load();
		std::FILE *f = nullptr;
		while (!log_quit_.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			const std::uint32_t h = log_head_.load(std::memory_order_acquire);
			const std::uint32_t s = pub_seq_.load(std::memory_order_acquire);
			const bool load_line = s != seq && log_on_.load(std::memory_order_relaxed);
			seq = s;
			if (h == tail && !load_line)
				continue;
			if (!f && (f = std::fopen(log_path_.c_str(), "a")) != nullptr)
				std::fprintf(f, "# KP Sim Prophetic MIDI log: seconds (host frames / rate), status, data; LOAD lines once a second\n");
			if (f && load_line)
				std::fprintf(f, "%10.4f LOAD avg %.2f peak %.2f asleep %.0f cpu %d\n", pub_time_.load(), pub_avg_.load(), pub_peak_.load(),
				             pub_asleep_.load(), pub_cpu_.load());
			if (h - tail > kLogRing)
				tail = h - kLogRing;   // overrun: keep the newest
			for (; tail != h; tail++)
			{
				const std::uint32_t m = log_msg_[tail & (kLogRing - 1)];
				const int n = int(m >> 24);
				if (f)
				{
					std::fprintf(f, "%10.4f %02X", log_frame_[tail & (kLogRing - 1)] / log_rate_.load(), unsigned(m & 0xff));
					if (n > 1) std::fprintf(f, " %02X", unsigned((m >> 8) & 0xff));
					if (n > 2) std::fprintf(f, " %02X", unsigned((m >> 16) & 0xff));
					std::fprintf(f, "\n");
				}
			}
			if (f)
				std::fflush(f);
		}
		if (f)
			std::fclose(f);
	}

	void handle(const mpc::MidiEvent &ev)
	{
		log_midi(ev);
		const int st = ev.status();
		if (ev.isNoteOn())
			last_key_ = ev.note() & 127;   // the key polyphonic pressure follows (the Prophecy is monophonic)
		if (st == 0xa0)
		{
			// polyphonic aftertouch (the MPC's pads): one voice: the pressure on the last key played is
			// the Prophecy's aftertouch; more voices: each pad's pressure goes to the voice playing it
			kp::Synth::KnobMoves m;
			if (synth_.voices() > 1)
				synth_.poly_pressure(ev.data[1] & 127, ev.data[2], &m);
			else if ((ev.data[1] & 127) == last_key_)
				synth_.aftertouch(ev.data[2], &m);
			apply_moves(m);
			return;
		}
		if (ev.isNoteOn())
		{
			gate_target_ = 1.0f;   // a note after a Stop: sound again
			synth_.set_muted(false);
			if (!arp_.note_on(arp_set_, ev.note(), ev.velocity()))
			{
				synth_.note_on(ev.note(), ev.velocity());
				held_[ev.note() & 127] = true;
			}
		}
		else if (st == 0x80 || (st == 0x90 && ev.velocity() == 0))
		{
			if (!arp_.note_off(arp_set_, ev.note()))
			{
				synth_.note_off(ev.note(), st == 0x80 ? ev.data[2] : 0);   // release velocity
				held_[ev.note() & 127] = false;
			}
		}
		else if (ev.isAllNotesOff())
		{
			stop_arp();
			for (int n = 0; n < 128; n++)
				if (held_[n]) { synth_.note_off(n); held_[n] = false; }
			synth_.midi_cc(ev.data[1], 0);   // and the Prophecy's own All Notes Off / All Sound Off
		}
		else if (ev.isController())
		{
			const int cc = ev.controller(), v = ev.controllerValue();
			if (cc == 32) bank_ = v;   // 0 A, 1 B, 2.. the user programs (64 per bank)
			else
			{
				if (cc == 1) mod_ = v;
				kp::Synth::KnobMoves m;   // CC19..23 (Knob Controller Assign) turn the PE knobs
				apply_nrpn(synth_.midi_cc(cc, v, &m));
				apply_moves(m);
			}
		}
		else if (ev.isPitchBend()) synth_.pitch_bend(ev.pitchBend() + 8192);
		else if (st == 0xd0)
		{
			kp::Synth::KnobMoves m;
			synth_.aftertouch(ev.data[1], &m);
			apply_moves(m);
		}
		else if (ev.isProgramChange())
		{
			const int p = bank_ * 64 + (ev.data[1] & 63);
			if (p < synth_.program_count())
				load_program(p, false);
		}
	}

	kp::PolySynth synth_;   // voices: one Prophecy engine each (Voices 1: the Prophecy itself)
	kp::Arp arp_;
	kp::Arp::Settings arp_set_;
	std::array<kp::Arp::PatternData, kp::Arp::kPatterns> arp_patterns_{};   // from ic12 (UP..RANDOM, PAT1-5)
	int arp_pattern_ = -1;    // pattern the arpeggiator settings belong to (-1: adopt the current one)
	std::array<std::array<std::uint8_t, 128>, kp::Arp::kPatterns> pattern_bytes_{};   // the 10 patterns (edited)
	std::vector<std::array<std::uint8_t, 9>> preset_assign_;   // per program: Controller Assignment
	int ca_shown_[9] = { 1, 6, 7, 8, 21, 22, 23, 1, 0 };      // Controller Assignment the engine has
	int pe_sel_[2] = { -1, -1 }, pe_shown_[4] = {};
	bool pe_refresh_ = true, pat_refresh_ = true;
	int pat_sel_[2] = { -1, -1 };
	std::array<int, 6> pat_shown_{};
	std::array<std::uint8_t, kExtraV2 - 8> staged_extra_{};
	std::atomic<bool> staged_valid_{ false };
	int pattern_load_ = -1;   // pattern whose stored settings load on the next block
	Resampler rs_;
	std::vector<float> scratchL_, scratchR_;
	std::vector<std::vector<int>> preset_raw_;   // per program: raw parameter values after loading it
	std::vector<kp::pe::Program> preset_perf_;   // per program: Performance Editor settings
	int baseline_[kp::prm::kCount] = {};          // raw values the engine holds
	int knob_pos_[kp::pe::kKnobs] = {};          // Performance Editor state the engine holds
	bool pe_on_[kp::pe::kSets] = {};
	bool ok_ = false;
	bool restoring_ = false;
	std::atomic<int> restore_program_{ -1 };
	std::atomic<bool> panic_{ false };   // host reset: silence on the audio thread
	float gate_ = 1.0f, gate_target_ = 1.0f;
	int applied_program_ = -1;   // program the engine has (edits are relative to it)
	int host_program_ = -1;      // last host preset seen (MIDI program changes do not move it)
	int bank_ = 0, mod_ = 0, transpose_ = 0;
	int master_tune_ = -1000, dlyrev_ = -1, user_scale_[12] = { -1000, -1000, -1000, -1000, -1000, -1000,
	                                                            -1000, -1000, -1000, -1000, -1000, -1000 };   // unsent
	std::array<int, 128> user_scale2_ = make_unsent();
	int transpose_pos_ = -1;
	int knob_ctl_[kp::pe::kKnobs] = { 5 + 19, 5 + 20, 5 + 21, 5 + 22, 5 + 23 };   // what the engine has
	static std::array<int, 128> make_unsent() { std::array<int, 128> a; a.fill(-1000); return a; }
	// Presets carry a Volume value (parameter 0); the SDK switches the program number first and then
	// writes the values, so a Volume write that comes with a new program number is a preset's.
	std::atomic<float> user_volume_{ 12.0f };
	std::atomic<bool> revert_volume_{ false };
	int ui_program_ = 0;
	bool porta_ = false;
	bool held_[128] = {};
	int last_key_ = -1;
	std::array<std::atomic<std::uint32_t>, 3> midi_log_{};   // status | d1 << 8 | d2 << 16 | size << 24
	int midi_count_ = 0;
	static constexpr std::uint32_t kLogRing = 8192;
	std::array<std::uint32_t, kLogRing> log_msg_{}, log_frame_{};
	std::atomic<std::uint32_t> log_head_{ 0 };
	std::atomic<bool> log_on_{ false }, log_quit_{ false };
	std::atomic<double> log_rate_{ 44100.0 };
	std::uint64_t frames_ = 0;
	bool log_playing_ = false;
	double log_ppq_ = -1.0;
	std::string log_path_;
	std::thread log_thread_;

public:
	~Prophecy() override
	{
		log_quit_.store(true);
		if (log_thread_.joinable())
			log_thread_.join();
	}

private:
	double worst_load_ = 0, avg_load_ = 0;
	double acc_used_ = 0, acc_real_ = 0, acc_peak_ = 0, acc_asleep_ = 0;   // the current second (audio thread)
	std::atomic<double> pub_avg_{ 0 }, pub_peak_{ 0 }, pub_asleep_{ 0 }, pub_time_{ 0 };   // the last full second
	std::atomic<int> pub_cpu_{ -1 };
	std::atomic<std::uint32_t> pub_seq_{ 0 };
};

// The SDK's entry, plus one answer it does not give: the MPC asks plugins whether they take MPE (per-note
// pitch bend / pressure / slide, how multi-dimensional pads reach a synth); this plugin says yes.
namespace {
AEffectDispatcherProc g_sdk_dispatcher = nullptr;
VstIntPtr prophecy_dispatcher(AEffect *e, VstInt32 op, VstInt32 index, VstIntPtr value, void *ptr, float opt)
{
	if (op == effCanDo && ptr != nullptr && std::strcmp(static_cast<const char *>(ptr), "MPE") == 0)
		return 1;
	return g_sdk_dispatcher(e, op, index, value, ptr, opt);
}
} // namespace

extern "C" MPC_EXPORT AEffect *VSTPluginMain(audioMasterCallback host)
{
	if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0)
		return nullptr;   // host is VST1 only
	AEffect *e = mpc::detail::createEffect(new Prophecy(), host);
	if (e != nullptr)
	{
		g_sdk_dispatcher = e->dispatcher;
		e->dispatcher = &prophecy_dispatcher;
	}
	return e;
}
