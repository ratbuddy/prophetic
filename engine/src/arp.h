// Arpeggiator. On the Prophecy it runs on the panel CPU (V55), which sends the H8 plain note
// messages; this is a native implementation of the Performance-mode arpeggiator, checked against
// the oracle's V55 (reference/wsl/arp_expect.py, engine/test/arp_test.cpp):
//   preset patterns UP / DOWN / ALT1 (up and down, ends once) / ALT2 (ends repeated) / RANDOM,
//   OCTAVE 1-4 (each pass through the notes goes up an octave), and the five user patterns PAT1-5
//   (Parameter Guide, Pattern Define): up to 24 steps of {Tone 1-12 or LOOP, velocity, gate 0 (rest)
//   ..100 %, timing offset -49..+49 % of a step}; Arpeggio Type decides a tone beyond the held notes
//   (As Played: rest, Fill: the last note, Running Up: wrap, Up&Down: reflect); the octave moves
//   on each pattern loop (Octave Alternation Up / Down / Up&Down);
//   LATCH, KEY SYNC, Step Base, Gate (% or STEP), Velocity (KEY, fixed, or STEP), Sort, scan zone.
// As on the V55: at 100 % gate a step on the note already sounding extends it (no new note-on), a
// new note's note-on precedes the old one's note-off, and a note-off carries the velocity of the
// step that ends it. A controller (Global Arpeggiator Controller Assign; its value in vel_ctrl /
// gate_ctrl) moves each step's velocity / gate toward the maximum or the minimum by the pattern's
// Controller Intensity, with the V55's integer arithmetic (its step routine, ROM 0xB3600):
// x = intensity * value / 32767; v += x * (x >= 0 ? 127 - v : v) / 99 (velocity 0: no note);
// g += x * (x >= 0 ? 100 - g : g) / 99, kept in 1..100.
//
// Timing: locked to the host's musical position while its transport plays (steps on the beat
// grid), otherwise free-running at the host tempo (or `bpm` when the host gives none); the clock
// keeps running between chords. A new arpeggio starts at step 1; with KEY SYNC Off (the Prophecy's
// default) its first step waits for the clock's next step, with KEY SYNC On it sounds at the key
// and the clock restarts there (measured on the oracle). Events are placed at block granularity.
// Not reproduced: when a key arrives within milliseconds of switching the arpeggiator on, the
// V55 first plays a burst of steps about 1.3 ms apart (its timer catching up).
#pragma once

#include <array>
#include <cstdint>

namespace kp {

class Arp
{
public:
	enum Pattern { kUp, kDown, kAlt1, kAlt2, kRandom, kPat1, kPat2, kPat3, kPat4, kPat5, kPatterns };
	enum Step { k4, k4T, k8, k8T, k16, k16T };   // Step Base choices
	enum Type { kAsPlayed, kFill, kRunningUp, kUpDown };
	enum OctAlt { kOctUp, kOctDown, kOctUpDown };
	static constexpr int kVelKey = 0, kVelStep = 128;   // Settings::velocity: 0 KEY, 1..127 fixed, 128 STEP
	static constexpr int kGateStep = 101;               // Settings::gate: 1..100 %, 101 STEP
	static constexpr int kSteps = 24, kLoop = 13;

	// One of the Prophecy's 10 internal arpeggio patterns (128 bytes, Parameter Guide 6.6
	// "Arpeggio Pattern Parameters"); the five presets carry only the base settings.
	struct PatternData
	{
		int step = k8;
		bool sort = true;
		int zone_lo = 0, zone_hi = 127;
		int velocity = kVelKey;
		int gate = 80;
		int type = kRunningUp;
		int oct_alt = kOctUp;
		int vel_int = 99, gate_int = 99;   // Velocity / Gate Controller Intensity, -99..+99
		struct S { int offset = 0, tone = 0, velocity = 100, gate = 80; } steps[kSteps];
		bool valid = false;
		static PatternData from_bytes(const std::uint8_t *b);   // b: 128 bytes
	};

	struct Settings
	{
		bool on = false;
		int pattern = kUp;
		int octaves = 1;      // 1..4
		bool latch = false;
		bool key_sync = false;   // On: the first step at the key (clock restarts); Off: at the clock's next step
		int step = k8;
		int gate = 80;        // 1..100 %, kGateStep
		int velocity = kVelKey;
		bool sort = true;     // ON: by pitch; OFF: in the order played
		int zone_lo = 0, zone_hi = 127;   // keyboard scan zone (notes outside play normally)
		double bpm = 120;     // used when the host has no tempo
		int vel_ctrl = 0, gate_ctrl = 0;   // assigned controllers' values, -32768..32767 (0: none assigned)
		int vel_int = 99, gate_int = 99;   // the pattern's Controller Intensities
		const PatternData *user = nullptr;   // steps of the selected user pattern (pattern >= kPat1)
	};

	struct Out   // one note event for the synth
	{
		bool on;
		int note, velocity;
	};

	// Note input. Returns false when the note is not taken by the arpeggiator (arp off, or outside
	// the scan zone): play it directly.
	bool note_on(const Settings &s, int note, int velocity);
	bool note_off(const Settings &s, int note);

	// Advance `frames` at `rate`. tempo <= 0: none from the host; ppq < 0: transport not playing.
	// Calls emit(Out) for the notes to send, in order. Returns the number of events emitted.
	template <class Emit>
	int run(const Settings &s, int frames, double rate, double tempo, double ppq, Emit emit)
	{
		Out ev[32];
		const int n = advance(s, frames, rate, tempo, ppq, ev, 32);
		for (int i = 0; i < n; i++)
			emit(ev[i]);
		return n;
	}

	// Release everything (arp switched off, all notes off, latch released).
	template <class Emit>
	void stop(Emit emit)
	{
		if (sounding_ >= 0)
			emit(Out{ false, sounding_, sound_vel_ });
		sounding_ = -1;
		count_ = 0;
		held_ = 0;
		started_ = false;
	}

	bool active() const { return count_ > 0; }

private:
	struct Note { int note = -1, velocity = 0; };
	int advance(const Settings &s, int frames, double rate, double tempo, double ppq, Out *ev, int max);
	Note step_note(const Settings &s, int *gate);   // the pending step (note -1: rest)
	void controllers(const Settings &s, Note &nt, int &gate) const;   // velocity / gate controllers
	void next_step(const Settings &s);              // move the sequence on by one step
	int pending_offset(const Settings &s) const;     // timing offset of the pending step, %
	bool user(const Settings &s) const { return s.pattern >= kPat1 && s.user && s.user->valid; }
	void remove(int note);
	void order(const Settings &s, int *idx) const;

	static constexpr int kMax = 32;
	std::array<int, kMax> notes_{}, vels_{};   // arpeggio notes, in the order played
	int count_ = 0;       // notes in the arpeggio
	int held_ = 0;        // keys physically held (latch keeps the notes after release)
	bool restart_ = false;   // first note of a new arpeggio: (re)start
	bool started_ = false;
	// sequence position
	int seq_ = 0;         // preset: index in the multi-octave sequence; user: step 0..23
	int cycle_ = 0;       // user: pattern repetitions (octave)
	// time, in steps
	double pos_ = 0;      // free-running position
	double next_on_ = 0;  // when the pending step sounds
	double off_at_ = 0;   // when the sounding note's gate closes (huge: legato, until the next note)
	long long grid_ = 0;  // nominal index of the pending step
	int sounding_ = -1, sound_vel_ = 0;   // note currently on (sent) and its current step velocity
	std::uint32_t rng_ = 0x1234567u;
};

} // namespace kp
