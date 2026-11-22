#include "poly.h"

namespace kp {

PolySynth::PolySynth()
{
	for (auto &v : v_)
		v = std::make_unique<Synth>();
	note_.fill(-1);
}

PolySynth::~PolySynth()
{
	quit_.store(true);
	if (loader_.joinable())
		loader_.join();
}

bool PolySynth::load(const std::string &dir, std::string &error)
{
	if (!v_[0]->load(dir, error))
		return false;
	if (v_[0]->booted())
		v_[0]->save_snapshot(dir);   // the other voices load it instead of booting again
	dir_ = dir;
	loaded_.store(1, std::memory_order_release);
	return true;
}

int PolySynth::load_user_sysex(const std::string &dir, std::string *report)
{
	user_dir_ = dir;
	return v_[0]->load_user_sysex(dir, report);
}

void PolySynth::load_rest()
{
	for (int k = loaded_.load(); k < kMaxVoices && !quit_.load(); k++)
	{
		std::string err;
		if (!v_[k]->load(dir_, err))
			return;
		if (!user_dir_.empty())
			v_[k]->load_user_sysex(user_dir_, nullptr);
		loaded_.store(k + 1, std::memory_order_release);   // voice k is ready for the audio thread
	}
}

void PolySynth::load_voices_async()
{
	if (loaded_.load() < 1 || loader_.joinable())
		return;
	loader_ = std::thread([this] { load_rest(); });
}

void PolySynth::load_all()
{
	if (loader_.joinable())
		loader_.join();
	load_rest();
}

void PolySynth::set_voices(int n)
{
	const int have = loaded_.load(std::memory_order_acquire);
	n = n < 1 ? 1 : (n > have ? have : n);
	for (int k = n; k < voices_; k++)   // dropped: released, silenced, faded out (render)
	{
		if (held_[k])
			v_[k]->note_off(note_[k]);
		held_[k] = false;
		note_[k] = -1;
		v_[k]->midi_cc(123, 0);
		v_[k]->midi_cc(120, 0);
		if (fade_[k] == 0)
			fading_++;
		fade_[k] = kFadeFrames;
	}
	for (int k = voices_; k < n; k++)   // added (the caller brings them up to date)
	{
		if (fade_[k] > 0)
		{
			fade_[k] = 0;
			fading_--;
		}
		when_[k] = ++clock_;   // as if just released: a free voice that is ready takes the next note first
	}
	voices_ = n;
	from_ = 0;
}

void PolySynth::match_controllers(int first)
{
	for (int k = first < 1 ? 1 : first; k < voices_; k++)
		v_[k]->match_controllers(*v_[0]);
}

void PolySynth::note_on(int note, int velocity)
{
	if (voices_ == 1)
	{
		v_[0]->note_on(note, velocity);
		return;
	}
	int k = -1;
	for (int i = 0; i < voices_; i++)   // the same key again: its voice retriggers
		if (held_[i] && note_[i] == note)
		{
			k = i;
			break;
		}
	if (k < 0)
		for (int i = 0; i < voices_; i++)   // a free voice: the one released longest ago
			if (!held_[i] && (k < 0 || when_[i] < when_[k]))
				k = i;
	if (k < 0)   // every voice held: the one holding its note longest
	{
		k = 0;
		for (int i = 1; i < voices_; i++)
			if (when_[i] < when_[k])
				k = i;
		v_[k]->note_off(note_[k]);
	}
	v_[k]->note_on(note, velocity);
	note_[k] = note;
	held_[k] = true;
	when_[k] = ++clock_;
}

void PolySynth::note_off(int note, int velocity)
{
	if (voices_ == 1)
	{
		v_[0]->note_off(note, velocity);
		return;
	}
	for (int k = 0; k < voices_; k++)
		if (held_[k] && note_[k] == note)
		{
			v_[k]->note_off(note, velocity);
			held_[k] = false;
			when_[k] = ++clock_;
			return;
		}
}

void PolySynth::poly_pressure(int note, int value, Synth::KnobMoves *moved)
{
	if (voices_ == 1)
	{
		v_[0]->aftertouch(value, moved);
		return;
	}
	if (moved)
		moved->count = 0;
	for (int k = 0; k < voices_; k++)
		if (held_[k] && note_[k] == note)
		{
			v_[k]->aftertouch(value, k == 0 ? moved : nullptr);
			return;
		}
}

#define KP_ALL(call)                         \
	for (int k = from_; k < voices_; k++)    \
		v_[k]->call;

void PolySynth::program(int index) { KP_ALL(program(index)) }
void PolySynth::set_param(int index, int raw) { KP_ALL(set_param(index, raw)) }
void PolySynth::set_program_extra(int offset, int value) { KP_ALL(set_program_extra(offset, value)) }
void PolySynth::set_user_scale(int scale, int key, int cents) { KP_ALL(set_user_scale(scale, key, cents)) }
void PolySynth::set_transpose(int semitones) { KP_ALL(set_transpose(semitones)) }
void PolySynth::set_transpose_position(int pos) { KP_ALL(set_transpose_position(pos)) }
void PolySynth::set_master_tune(int tenths_hz) { KP_ALL(set_master_tune(tenths_hz)) }
void PolySynth::set_delay_reverb(bool on) { KP_ALL(set_delay_reverb(on)) }
void PolySynth::set_knob_assign(int knob, int code) { KP_ALL(set_knob_assign(knob, code)) }
void PolySynth::set_perf_position(int knob, int value) { KP_ALL(set_perf_position(knob, value)) }
void PolySynth::pe_switch(int set, bool on) { KP_ALL(pe_switch(set, on)) }
void PolySynth::set_idle_sleep(bool on) { KP_ALL(set_idle_sleep(on)) }
void PolySynth::set_muted(bool on) { KP_ALL(set_muted(on)) }
void PolySynth::pitch_bend(int value14) { KP_ALL(pitch_bend(value14)) }

#undef KP_ALL

void PolySynth::aftertouch(int value, Synth::KnobMoves *moved)
{
	v_[from_]->aftertouch(value, moved);
	for (int k = from_ + 1; k < voices_; k++)
		v_[k]->aftertouch(value, nullptr);
}

Synth::Nrpn PolySynth::midi_cc(int cc, int value, Synth::KnobMoves *moved)
{
	const Synth::Nrpn n = v_[from_]->midi_cc(cc, value, moved);
	for (int k = from_ + 1; k < voices_; k++)
		v_[k]->midi_cc(cc, value, nullptr);
	return n;
}

int PolySynth::perf_knob(int knob, int value, pe::Edit out[pe::kSets])
{
	const int n = v_[from_]->perf_knob(knob, value, out);
	pe::Edit scratch[pe::kSets];
	for (int k = from_ + 1; k < voices_; k++)
		v_[k]->perf_knob(knob, value, scratch);
	return n;
}

void PolySynth::render(float *left, float *right, int n)
{
	if (voices_ == 1 && fading_ == 0)
	{
		v_[0]->render(left, right, n);
		return;
	}
	for (int off = 0; off < n;)
	{
		const int c = n - off < int(mix_l_.size()) ? n - off : int(mix_l_.size());
		v_[0]->render(left + off, right + off, c);
		for (int k = 1; k < voices_; k++)
		{
			v_[k]->render(mix_l_.data(), mix_r_.data(), c);
			for (int i = 0; i < c; i++)
			{
				left[off + i] += mix_l_[size_t(i)];
				right[off + i] += mix_r_[size_t(i)];
			}
		}
		for (int k = voices_; fading_ > 0 && k < kMaxVoices; k++)   // dropped voices: a linear fade to silence
		{
			if (fade_[k] == 0)
				continue;
			v_[k]->render(mix_l_.data(), mix_r_.data(), c);
			for (int i = 0; i < c && fade_[k] > 0; i++)
			{
				const float g = float(fade_[k]--) / float(kFadeFrames);
				left[off + i] += g * mix_l_[size_t(i)];
				right[off + i] += g * mix_r_[size_t(i)];
			}
			if (fade_[k] == 0)
				fading_--;
		}
		off += c;
	}
}

bool PolySynth::sleeping() const
{
	for (int k = 0; k < voices_; k++)
		if (!v_[k]->sleeping())
			return false;
	return true;
}

} // namespace kp
