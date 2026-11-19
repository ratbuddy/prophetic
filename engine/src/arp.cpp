#include "arp.h"

#include <algorithm>
#include <cmath>

namespace kp {

namespace {

constexpr double kLegato = 1e30;

double step_quarters(int step)   // length of one step in quarter notes
{
	static const double q[6] = { 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0 };
	return q[std::clamp(step, 0, 5)];
}

} // namespace

Arp::PatternData Arp::PatternData::from_bytes(const std::uint8_t *b)
{
	PatternData p;
	p.step = std::clamp(int(b[17]), 0, 5);
	p.sort = b[18] != 0;
	p.zone_lo = b[19] & 0x7f;
	p.zone_hi = b[20] & 0x7f;
	p.velocity = b[21] == 128 ? kVelKey : (b[21] == 129 ? kVelStep : std::clamp(int(b[21]), 1, 127));
	p.gate = std::clamp(int(b[24]), 1, kGateStep);
	p.type = std::clamp(int(b[27]), 0, 3);
	p.oct_alt = std::clamp(int(b[28]), 0, 2);
	p.vel_int = std::clamp(int(std::int8_t(b[23])), -99, 99);
	p.gate_int = std::clamp(int(std::int8_t(b[26])), -99, 99);
	for (int k = 0; k < kSteps; k++)
	{
		const std::uint8_t *st = b + 32 + 4 * k;
		p.steps[k] = { int(std::int8_t(st[0])), int(st[1]), int(st[2]), int(st[3]) };
	}
	p.valid = true;
	return p;
}

bool Arp::note_on(const Settings &s, int note, int velocity)
{
	if (!s.on || note < s.zone_lo || note > s.zone_hi)
		return false;
	if (held_ == 0 && s.latch)
		count_ = 0;                    // a new chord replaces the latched one
	const bool first = count_ == 0;
	remove(note);
	if (count_ < kMax)
	{
		notes_[count_] = note;
		vels_[count_] = velocity;
		count_++;
	}
	held_++;
	if (first)
	{
		restart_ = true;               // a new arpeggio starts at step 1 (Key Sync On or Off)
		seq_ = 0;
		cycle_ = 0;
	}
	return true;
}

bool Arp::note_off(const Settings &s, int note)
{
	if (!s.on || note < s.zone_lo || note > s.zone_hi)
		return false;
	if (held_ > 0)
		held_--;
	if (!s.latch)
		remove(note);
	return true;
}

void Arp::remove(int note)
{
	for (int i = 0; i < count_; i++)
		if (notes_[i] == note)
		{
			for (int k = i; k + 1 < count_; k++)
			{
				notes_[k] = notes_[k + 1];
				vels_[k] = vels_[k + 1];
			}
			count_--;
			return;
		}
}

void Arp::order(const Settings &s, int *idx) const   // note indices: by pitch (Sort ON) or as played
{
	for (int i = 0; i < count_; i++)
		idx[i] = i;
	if (s.sort)
		std::stable_sort(idx, idx + count_, [this](int a, int b) { return notes_[a] < notes_[b]; });
}

int Arp::pending_offset(const Settings &s) const
{
	if (!user(s))
		return 0;
	const auto &st = s.user->steps[std::clamp(seq_, 0, kSteps - 1)];
	return std::clamp(st.offset, -49, 49);
}

void Arp::next_step(const Settings &s)
{
	if (user(s))
	{
		// LOOP (or the end of the 24 steps) returns to step 1 in that step's time; the octave
		// moves on with each repetition
		seq_++;
		if (seq_ >= kSteps || s.user->steps[seq_].tone == kLoop)
		{
			seq_ = 0;
			cycle_++;
		}
		return;
	}
	seq_++;
}

Arp::Note Arp::step_note(const Settings &s, int *gate)
{
	Note r;
	int idx[kMax];
	order(s, idx);
	const int n = count_, oct = std::clamp(s.octaves, 1, 4);
	if (user(s))
	{
		const PatternData &p = *s.user;
		const auto &st = p.steps[std::clamp(seq_, 0, kSteps - 1)];
		*gate = s.gate == kGateStep ? std::clamp(st.gate, 0, 100) : std::clamp(s.gate, 1, 100);
		int i = st.tone - 1;
		if (st.tone < 1 || st.tone >= kLoop || *gate == 0)
			return r;   // rest
		if (i >= n)
		{
			switch (p.type)
			{
			case kAsPlayed: return r;   // rest
			case kFill: i = n - 1; break;
			case kRunningUp: i %= n; break;
			default:
			{
				const int per = 2 * n - 2;
				i = per > 0 ? i % per : 0;
				if (i >= n) i = per - i;
				break;
			}
			}
		}
		int o = 0;
		switch (p.oct_alt)
		{
		case kOctDown: o = oct - 1 - cycle_ % oct; break;
		case kOctUpDown:
		{
			const int per = 2 * oct - 2;
			o = per > 0 ? cycle_ % per : 0;
			if (o >= oct) o = per - o;
			break;
		}
		default: o = cycle_ % oct; break;
		}
		const int base = idx[i];
		r.note = std::clamp(notes_[base] + 12 * o, 0, 127);
		r.velocity = s.velocity == kVelStep ? st.velocity : (s.velocity == kVelKey ? vels_[base] : s.velocity);
		r.velocity = std::clamp(r.velocity, 1, 127);
		return r;
	}
	// preset patterns over the ascending multi-octave sequence 0..len-1
	*gate = std::clamp(s.gate, 1, 100);
	const int len = n * oct;
	int seqlen = len;
	if (s.pattern == kAlt1) seqlen = len > 1 ? 2 * len - 2 : len;
	if (s.pattern == kAlt2) seqlen = 2 * len;
	if (seq_ >= seqlen)
		seq_ = 0;
	const int k = seq_;
	int x;
	switch (s.pattern)
	{
	case kDown: x = len - 1 - k; break;
	case kAlt1: x = k < len ? k : 2 * len - 2 - k; break;
	case kAlt2: x = k < len ? k : 2 * len - 1 - k; break;
	case kRandom:
		rng_ = rng_ * 1664525u + 1013904223u;
		x = int((rng_ >> 8) % unsigned(len));
		break;
	default: x = k; break;
	}
	x = std::clamp(x, 0, len - 1);
	const int base = idx[x % n];
	r.note = std::clamp(notes_[base] + 12 * (x / n), 0, 127);
	r.velocity = std::clamp(s.velocity == kVelKey || s.velocity == kVelStep ? vels_[base] : s.velocity, 1, 127);
	return r;
}

void Arp::controllers(const Settings &s, Note &nt, int &gate) const
{
	if (nt.note < 0 || gate == 0)
		return;
	const int vx = std::clamp(s.vel_int, -99, 99) * std::clamp(s.vel_ctrl, -32768, 32767) / 32767;
	int v = nt.velocity + vx * (vx >= 0 ? 127 - nt.velocity : nt.velocity) / 99;
	if (v <= 0)
		nt.note = -1;   // velocity 0: the step does not sound
	nt.velocity = std::clamp(v, 1, 127);
	const int gx = std::clamp(s.gate_int, -99, 99) * std::clamp(s.gate_ctrl, -32768, 32767) / 32767;
	gate = std::clamp(gate + gx * (gx >= 0 ? 100 - gate : gate) / 99, 1, 100);
}

int Arp::advance(const Settings &s, int frames, double rate, double tempo, double ppq, Out *ev, int max)
{
	int n = 0;
	auto push = [&](bool on, int note, int vel) { if (n < max) ev[n++] = Out{ on, note, vel }; };
	auto release = [&]() {
		if (sounding_ >= 0)
			push(false, sounding_, sound_vel_);
		sounding_ = -1;
		off_at_ = kLegato;
	};
	const double bpm = tempo > 0 ? tempo : s.bpm;
	const double q = step_quarters(s.step);
	const bool host = ppq >= 0 && tempo > 0;
	double p0, p1;   // position at the start / end of this block, in steps
	if (host)
	{
		p0 = ppq / q;
		p1 = (ppq + frames / rate * bpm / 60.0) / q;
	}
	else
	{
		p0 = pos_;
		p1 = pos_ + frames / rate * bpm / 60.0 / q;
	}
	if (count_ == 0)
	{
		release();
		restart_ = false;
		if (!host)
			pos_ = p1;                  // the arpeggiator clock keeps running between chords
		return n;
	}
	if (restart_)
	{
		restart_ = false;
		started_ = true;
		if (s.key_sync)
		{
			// Key Sync On: the first step sounds at the key and the clock restarts there
			if (!host)
				p1 -= p0, p0 = 0;
			grid_ = (long long)std::floor(p0 + 1e-9);
			next_on_ = p0;
		}
		else
		{
			// Key Sync Off: the first step waits for the clock's next step (as the V55 does)
			grid_ = (long long)std::ceil(p0 - 1e-9);   // a key on a step sounds on it
			next_on_ = std::max(p0, grid_ + pending_offset(s) / 100.0);
		}
		off_at_ = kLegato;
	}
	else if (next_on_ > p1 + 2 || next_on_ < p0 - 2)   // host jumped (loop, locate): follow the grid
	{
		grid_ = (long long)std::floor(p0 + 1e-9) + 1;
		next_on_ = grid_ + pending_offset(s) / 100.0;
	}
	for (;;)
	{
		const bool off_first = sounding_ >= 0 && off_at_ <= next_on_;
		if (off_first && off_at_ < p1)
		{
			release();
			continue;
		}
		if (next_on_ >= p1)
			break;
		const double t = next_on_;
		int gate = 80;
		Note nt = step_note(s, &gate);
		controllers(s, nt, gate);
		const double off = gate >= 100 ? kLegato : t + gate / 100.0;
		if (nt.note < 0)
		{
			if (sounding_ >= 0 && off_at_ >= kLegato)   // a rest ends a legato note
				release();
		}
		else if (nt.note == sounding_)
		{
			sound_vel_ = nt.velocity;   // same note, still on: extend it
			off_at_ = off;
		}
		else
		{
			const int prev = sounding_, prev_vel = sound_vel_;
			push(true, nt.note, nt.velocity);   // note-on first: legato for the mono engine
			if (prev >= 0)
				push(false, prev, prev_vel);
			sounding_ = nt.note;
			sound_vel_ = nt.velocity;
			off_at_ = off;
		}
		next_step(s);
		grid_++;
		next_on_ = std::max(grid_ + pending_offset(s) / 100.0, t + 1e-6);
	}
	if (!host)
		pos_ = p1;
	return n;
}

} // namespace kp
