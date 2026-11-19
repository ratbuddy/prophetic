// Prophetic (KP Sim) - Windows test app: the native engine with a minimal UI.
//   Programs list (click to load), parameter list + value slider (edit the selected parameter),
//   PC keyboard as piano, MIDI input (first device), 48 kHz waveOut.
//   Keys: Z S X D C V G B H N J M = C..B (low row), Q 2 W 3 E R 5 T 6 Y 7 U I = C..C (high row),
//   Left/Right = octave down/up, Up/Down / mouse wheel = mod wheel, F9 = record.
//   Arpeggiator: F1 on/off, F2 pattern (Up..Random, Pat1-5), F3 octave, F4 latch, F5 step,
//   F6/F7 tempo -/+ (or NRPN).
//   Performance Editor: the PERF rows at the top of the parameter list, or MIDI CC19..23 / NRPN.
// Usage: prophecy_win.exe [data dir]   (default: <exe dir>\data; SysEx libraries in <data dir>\User\*.syx)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>

#include "arp.h"
#include "synth.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

kp::Synth g_synth;
HWND g_main = nullptr, g_list = nullptr, g_status = nullptr, g_volume = nullptr, g_mod = nullptr;
HWND g_params = nullptr, g_value = nullptr, g_value_label = nullptr;
int g_sel = -1;   // selected row of the parameter list (kPerfRows Performance Editor rows, then prm::kDefs)
constexpr int kPerfRows = kp::pe::kKnobs + kp::pe::kSets;
// Arpeggiator: notes from the keyboard / MIDI threads, clock from the audio thread.
std::mutex g_arp_mu;
kp::Arp g_arp;
kp::Arp::Settings g_arp_set;
std::array<kp::Arp::PatternData, kp::Arp::kPatterns> g_patterns{};   // from ic12 (UP..RANDOM, PAT1-5)

// select an arpeggio pattern with its own stored settings, as on the Prophecy (g_arp_mu held)
void select_pattern(kp::Arp::Settings &s, int pattern)
{
	s.pattern = pattern;
	const kp::Arp::PatternData &pd = g_patterns[size_t(pattern)];
	if (pd.valid)
	{
		s.step = pd.step;
		s.sort = pd.sort;
		s.zone_lo = pd.zone_lo;
		s.zone_hi = pd.zone_hi;
		s.velocity = pd.velocity;
		s.gate = pd.gate;
	}
	s.user = pattern >= kp::Arp::kPat1 ? &pd : nullptr;
}
int g_mod_value = 0;
std::atomic<bool> g_run{ true };
std::atomic<float> g_gain{ 4.0f };      // +12 dB: the DAC output alone leaves ~16-20 dB of headroom
std::atomic<int> g_underruns{ 0 };
std::atomic<int> g_cc_sent{ 0 };
std::atomic<bool> g_recording{ false };
std::vector<short> g_record;   // written by the audio thread while g_recording, saved on F9
int g_record_n = 0;
int g_volume_db = 12;
int g_octave = 0;
int g_program = 0;
std::string g_error;

constexpr int kFrames = 512, kBuffers = 8;   // 8 x 10.7 ms = 85 ms of queued audio

// Output stage: gain then a soft knee above 0.85 so loud programs round off instead of hard
// clipping (which itself sounds like crackle).
inline short to_pcm(float v, float gain)
{
	v *= gain;
	const float a = std::fabs(v);
	if (a > 0.85f)
		v = std::copysign(0.85f + 0.15f * std::tanh((a - 0.85f) / 0.15f), v);
	return short(v * 32767.0f);
}

void audio_thread()
{
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = kp::Synth::kSampleRate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 4;
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HWAVEOUT wo = nullptr;
	if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, DWORD_PTR(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
	{
		g_error = "waveOutOpen failed";
		return;
	}
	std::vector<std::vector<short>> pcm(kBuffers, std::vector<short>(kFrames * 2));
	std::vector<WAVEHDR> hdr(kBuffers);
	std::vector<float> l(kFrames), r(kFrames);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	auto fill = [&](int i) {
		{
			std::lock_guard<std::mutex> lk(g_arp_mu);
			g_arp.run(g_arp_set, kFrames, double(kp::Synth::kSampleRate), 0.0, -1.0, [](const kp::Arp::Out &o) {
				if (o.on) g_synth.note_on(o.note, o.velocity);
				else g_synth.note_off(o.note, o.velocity);
			});
		}
		g_synth.render(l.data(), r.data(), kFrames);
		const float g = g_gain.load();
		for (int k = 0; k < kFrames; k++)
		{
			pcm[i][2 * k] = to_pcm(l[k], g);
			pcm[i][2 * k + 1] = to_pcm(r[k], g);
		}
		if (g_recording)
			g_record.insert(g_record.end(), pcm[i].begin(), pcm[i].end());
		hdr[i] = WAVEHDR{};
		hdr[i].lpData = reinterpret_cast<LPSTR>(pcm[i].data());
		hdr[i].dwBufferLength = DWORD(pcm[i].size() * sizeof(short));
		waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
		waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
	};
	for (int i = 0; i < kBuffers; i++)
		fill(i);
	while (g_run)
	{
		WaitForSingleObject(ev, 100);
		int done = 0;
		for (int i = 0; i < kBuffers; i++)
			done += (hdr[i].dwFlags & WHDR_DONE) ? 1 : 0;
		if (done == kBuffers)
			g_underruns++;   // every queued buffer had already played: the device ran dry
		for (int i = 0; i < kBuffers; i++)
			if (hdr[i].dwFlags & WHDR_DONE)
			{
				waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
				fill(i);
			}
	}
	waveOutReset(wo);
	for (auto &h : hdr)
		waveOutUnprepareHeader(wo, &h, sizeof(WAVEHDR));
	waveOutClose(wo);
	CloseHandle(ev);
}

void play_on(int note, int vel)
{
	std::lock_guard<std::mutex> lk(g_arp_mu);
	if (!g_arp.note_on(g_arp_set, note, vel))
		g_synth.note_on(note, vel);
}

void play_off(int note, int vel = 0)
{
	std::lock_guard<std::mutex> lk(g_arp_mu);
	if (!g_arp.note_off(g_arp_set, note))
		g_synth.note_off(note, vel);
}

void CALLBACK midi_in(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR)
{
	if (msg != MIM_DATA)
		return;
	static int bank = 0;   // CC32 = 1 (with CC0 = 0) selects bank B, as on the hardware
	const int st = int(p1 & 0xf0), d1 = int((p1 >> 8) & 0x7f), d2 = int((p1 >> 16) & 0x7f);
	static int last_key = -1;   // polyphonic pressure follows the last key (the Prophecy is monophonic)
	if (st == 0x90 && d2 > 0) { last_key = d1; play_on(d1, d2); }
	else if (st == 0x80 || (st == 0x90 && d2 == 0)) play_off(d1, st == 0x80 ? d2 : 0);   // release velocity
	else if (st == 0xc0 && bank * 64 + (d1 & 63) < g_synth.program_count())
	{
		g_program = bank * 64 + (d1 & 63);
		g_synth.program(g_program);
		g_synth.midi_cc(1, g_mod_value);
		PostMessageA(g_main, WM_APP, 0, 0);
	}
	else if (st == 0xb0 && d1 == 32) bank = d2;   // 0 A, 1 B, 2.. the user programs (data\User\*.syx)
	else if (st == 0xb0)
	{
		const kp::Synth::Nrpn n = g_synth.midi_cc(d1, d2);
		if (n.number >= 1 && n.number <= 5)
		{
			std::lock_guard<std::mutex> lk(g_arp_mu);
			kp::Arp::Settings &s = g_arp_set;
			const bool on = n.step != 0 ? n.step > 0 : n.value >= 0x40;
			const bool was_on = s.on, was_latch = s.latch;
			if (n.number == 1) { const int v = n.step ? s.pattern + n.step : n.value; if (v >= 0 && v <= 9) select_pattern(s, v); }
			else if (n.number == 2) s.on = on;
			else if (n.number == 3) { const int v = n.step ? s.octaves + n.step : n.value + 1; if (v >= 1 && v <= 4) s.octaves = v; }
			else if (n.number == 4) s.latch = on;
			else s.key_sync = on;
			if ((was_on && !s.on) || (was_latch && !s.latch))
				g_arp.stop([](const kp::Arp::Out &o) { if (!o.on) g_synth.note_off(o.note, o.velocity); });
		}
		if ((d1 >= 19 && d1 <= 23) || n.number >= 6)
			PostMessageA(g_main, WM_APP, 0, 0);   // Performance Editor changed: refresh the list
	}
	else if (st == 0xe0) g_synth.pitch_bend(d1 | (d2 << 7));
	else if (st == 0xd0) g_synth.aftertouch(d1);
	else if (st == 0xa0 && d1 == last_key) g_synth.aftertouch(d2);   // poly pressure on the last key: aftertouch
}

int key_note(WPARAM k)
{
	static const char low[] = "ZSXDCVGBHNJM";
	static const char high[] = "Q2W3ER5T6Y7UI";
	for (int i = 0; low[i]; i++)
		if (WPARAM(low[i]) == k) return 48 + i + g_octave * 12;
	for (int i = 0; high[i]; i++)
		if (WPARAM(high[i]) == k) return 60 + i + g_octave * 12;
	return -1;
}

void update_status()
{
	static const char *const pat[] = { "Up", "Down", "Alt1", "Alt2", "Random", "Pat1", "Pat2", "Pat3", "Pat4", "Pat5" };
	static const char *const stp[] = { "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T" };
	char arp[96];
	{
		std::lock_guard<std::mutex> lk(g_arp_mu);
		std::snprintf(arp, sizeof arp, "Arp %s %s x%d %s %.0f bpm%s", g_arp_set.on ? "ON" : "off", pat[g_arp_set.pattern],
			g_arp_set.octaves, stp[g_arp_set.step], g_arp_set.bpm, g_arp_set.latch ? " latch" : "");
	}
	char buf[384];
	std::snprintf(buf, sizeof buf, "%s | Oct %+d | Vol %+d dB | Mod %d | %s | Load %.0f%% | Drop %d%s%s",
		g_synth.program_count() ? g_synth.program_name(g_program).c_str() : "(none)", g_octave, g_volume_db,
		g_mod_value, arp, g_synth.load_percent(), g_underruns.load(),
		g_recording ? " | REC" : "", g_error.empty() ? "" : (" | " + g_error).c_str());
	SetWindowTextA(g_status, buf);
}

// Parameter list: the Performance Editor (Knob 1-5, PE1-4), then one row per program parameter,
// "SECTION  Name = value".
std::string row_name(int r)
{
	if (r < kp::pe::kKnobs) return "Perf Knob " + std::to_string(r + 1);
	if (r < kPerfRows) return "PE" + std::to_string(r - kp::pe::kKnobs + 1);
	return kp::prm::kDefs[r - kPerfRows].name;
}
int row_value(int r)
{
	if (r < kp::pe::kKnobs) return g_synth.perf_knob_value(r);
	if (r < kPerfRows) return g_synth.pe_switch_on(r - kp::pe::kKnobs) ? 1 : 0;
	return g_synth.param(r - kPerfRows);
}
std::string row_text(int r)
{
	if (r < kp::pe::kKnobs) return std::to_string(row_value(r));
	if (r < kPerfRows) return row_value(r) ? "On" : "Off";
	return kp::prm::text(kp::prm::kDefs[r - kPerfRows], row_value(r));
}
std::string param_row(int r)
{
	if (r < kPerfRows)
		return "PERF   " + row_name(r) + " = " + row_text(r);
	const kp::prm::Def &d = kp::prm::kDefs[r - kPerfRows];
	std::string s = std::string(d.section) + "   " + d.name + " = " + row_text(r);
	if (!g_synth.param_active(r - kPerfRows))
		s += "   (model not active)";
	return s;
}

void show_value_label()
{
	if (g_sel < 0)
		return;
	SetWindowTextA(g_value_label, (row_name(g_sel) + ": " + row_text(g_sel)).c_str());
}

void refresh_params()
{
	SendMessageA(g_params, WM_SETREDRAW, FALSE, 0);
	const int top = int(SendMessageA(g_params, LB_GETTOPINDEX, 0, 0));
	SendMessageA(g_params, LB_RESETCONTENT, 0, 0);
	for (int i = 0; i < kPerfRows + kp::prm::kCount; i++)
		SendMessageA(g_params, LB_ADDSTRING, 0, LPARAM(param_row(i).c_str()));
	if (g_sel >= 0)
		SendMessageA(g_params, LB_SETCURSEL, g_sel, 0);
	SendMessageA(g_params, LB_SETTOPINDEX, top, 0);
	SendMessageA(g_params, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_params, nullptr, TRUE);
	if (g_sel >= 0)
		SendMessageA(g_value, TBM_SETPOS, TRUE, row_value(g_sel));
	show_value_label();
}

void refresh_row(int i)
{
	SendMessageA(g_params, LB_DELETESTRING, i, 0);
	SendMessageA(g_params, LB_INSERTSTRING, i, LPARAM(param_row(i).c_str()));
	SendMessageA(g_params, LB_SETCURSEL, g_sel, 0);
	show_value_label();
}

void select_param(int r)
{
	g_sel = r;
	int lo = 0, hi = r < kp::pe::kKnobs ? 127 : 1;
	if (r >= kPerfRows)
	{
		lo = kp::prm::kDefs[r - kPerfRows].lo;
		hi = kp::prm::kDefs[r - kPerfRows].hi;
	}
	SendMessageA(g_value, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
	SendMessageA(g_value, TBM_SETPOS, TRUE, row_value(r));
	show_value_label();
}

void arp_key(WPARAM k)
{
	std::lock_guard<std::mutex> lk(g_arp_mu);
	kp::Arp::Settings &s = g_arp_set;
	const bool was_on = s.on, was_latch = s.latch;
	if (k == VK_F1) s.on = !s.on;
	else if (k == VK_F2) select_pattern(s, (s.pattern + 1) % kp::Arp::kPatterns);
	else if (k == VK_F3) s.octaves = s.octaves % 4 + 1;
	else if (k == VK_F4) s.latch = !s.latch;
	else if (k == VK_F5) s.step = (s.step + 1) % 6;
	else if (k == VK_F6) s.bpm = std::max(40.0, s.bpm - 5);
	else if (k == VK_F7) s.bpm = std::min(300.0, s.bpm + 5);
	if ((was_on && !s.on) || (was_latch && !s.latch))
		g_arp.stop([](const kp::Arp::Out &o) { if (!o.on) g_synth.note_off(o.note, o.velocity); });
}

// Mod wheel from the slider, Up/Down keys or the mouse wheel: clamp, move the slider, send CC1.
void set_mod(int v)
{
	v = std::max(0, std::min(127, v));
	if (g_mod && int(SendMessageA(g_mod, TBM_GETPOS, 0, 0)) != v)
		SendMessageA(g_mod, TBM_SETPOS, TRUE, v);
	if (v != g_mod_value)
	{
		g_mod_value = v;
		g_synth.midi_cc(1, v);
		g_cc_sent++;
	}
}

// F9: start/stop recording the app's output to <exe dir>\record_N.wav (for checking by ear or tool).
void toggle_record()
{
	if (!g_recording)
	{
		g_record.clear();
		g_record.reserve(48000 * 2 * 60);
		g_recording = true;
		return;
	}
	g_recording = false;
	Sleep(50);   // let the audio thread finish its current block
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string path(exe);
	path = path.substr(0, path.find_last_of("\\/")) + "\\record_" + std::to_string(++g_record_n) + ".wav";
	if (FILE *f = std::fopen(path.c_str(), "wb"))
	{
		const unsigned bytes = unsigned(g_record.size() * 2), rate = 48000, brate = 48000 * 4, riff = 36 + bytes, sixteen = 16;
		const unsigned short fmt[] = { 1, 2 }, align[] = { 4, 16 };
		std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&sixteen, 4, 1, f);
		std::fwrite(fmt, 2, 2, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&brate, 4, 1, f); std::fwrite(align, 2, 2, f);
		std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f); std::fwrite(g_record.data(), 2, g_record.size(), f);
		std::fclose(f);
	}
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM lp)
{
	switch (m)
	{
	case WM_CREATE:
	{
		CreateWindowA("STATIC", "Keys: Z..M / Q..I play, Left/Right octave, Up/Down or wheel = mod, F9 record. Arp: F1 on, F2 pattern, F3 octave, F4 latch, F5 step, F6/F7 tempo.",
			WS_CHILD | WS_VISIBLE, 10, 8, 1040, 18, h, nullptr, nullptr, nullptr);
		g_params = CreateWindowA("LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
			630, 30, 420, 300, h, HMENU(4), nullptr, nullptr);
		g_value_label = CreateWindowA("STATIC", "Select a parameter", WS_CHILD | WS_VISIBLE, 630, 342, 420, 18, h, nullptr, nullptr, nullptr);
		g_value = CreateWindowA(TRACKBAR_CLASSA, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
			630, 368, 420, 28, h, HMENU(5), nullptr, nullptr);
		g_list = CreateWindowA("LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
			10, 30, 600, 300, h, HMENU(1), nullptr, nullptr);
		CreateWindowA("STATIC", "Volume", WS_CHILD | WS_VISIBLE, 10, 342, 70, 18, h, nullptr, nullptr, nullptr);
		g_volume = CreateWindowA(TRACKBAR_CLASSA, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
			80, 336, 530, 28, h, HMENU(2), nullptr, nullptr);
		SendMessageA(g_volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 36));   // -12 .. +24 dB
		SendMessageA(g_volume, TBM_SETPOS, TRUE, g_volume_db + 12);
		CreateWindowA("STATIC", "Mod wheel", WS_CHILD | WS_VISIBLE, 10, 374, 70, 18, h, nullptr, nullptr, nullptr);
		g_mod = CreateWindowA(TRACKBAR_CLASSA, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
			80, 368, 530, 28, h, HMENU(3), nullptr, nullptr);
		SendMessageA(g_mod, TBM_SETRANGE, TRUE, MAKELPARAM(0, 127));
		g_status = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 10, 404, 600, 18, h, nullptr, nullptr, nullptr);
		for (int i = 0; i < g_synth.program_count(); i++)
			SendMessageA(g_list, LB_ADDSTRING, 0, LPARAM(g_synth.program_name(i).c_str()));
		refresh_params();
		SetTimer(h, 1, 250, nullptr);
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD(w) == 1 && HIWORD(w) == LBN_SELCHANGE)
		{
			g_program = int(SendMessageA(g_list, LB_GETCURSEL, 0, 0));
			g_synth.program(g_program);
			g_synth.midi_cc(1, g_mod_value);   // the program burst resets the wheel; keep the slider's position
			refresh_params();
			SetFocus(h);
		}
		else if (LOWORD(w) == 4 && HIWORD(w) == LBN_SELCHANGE)
		{
			select_param(int(SendMessageA(g_params, LB_GETCURSEL, 0, 0)));
			SetFocus(h);
		}
		return 0;
	case WM_APP:   // program or Performance Editor changed from MIDI
		refresh_params();
		return 0;
	case WM_TIMER:
		update_status();
		return 0;
	case WM_HSCROLL:
		if (HWND(lp) == g_volume)
		{
			g_volume_db = int(SendMessageA(g_volume, TBM_GETPOS, 0, 0)) - 12;
			g_gain = std::pow(10.0f, float(g_volume_db) / 20.0f);
			SetFocus(h);
		}
		else if (HWND(lp) == g_value && g_sel >= 0)
		{
			const int v = int(SendMessageA(g_value, TBM_GETPOS, 0, 0));
			if (v != row_value(g_sel))
			{
				if (g_sel < kp::pe::kKnobs)
				{
					kp::pe::Edit e[kp::pe::kSets];
					g_synth.perf_knob(g_sel, v, e);
					refresh_params();   // the knob edits its assigned parameters
				}
				else if (g_sel < kPerfRows)
				{
					g_synth.pe_switch(g_sel - kp::pe::kKnobs, v != 0);
					refresh_row(g_sel);
				}
				else
				{
					g_synth.set_param(g_sel - kPerfRows, v);
					if (kp::prm::kDefs[g_sel - kPerfRows].id == kp::prm::kOscSetId)
						refresh_params();   // model parameters change with the oscillator set
					else
						refresh_row(g_sel);
				}
			}
			if (LOWORD(w) == TB_ENDTRACK) SetFocus(h);
		}
		else if (HWND(lp) == g_mod)
		{
			set_mod(int(SendMessageA(g_mod, TBM_GETPOS, 0, 0)));
			if (LOWORD(w) == TB_ENDTRACK) SetFocus(h);
		}
		return 0;
	case WM_MOUSEWHEEL:
		set_mod(g_mod_value + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 8 : -8));
		return 0;
	case WM_KEYDOWN:
		if (w == VK_LEFT && g_octave > -3) g_octave--;
		else if (w == VK_RIGHT && g_octave < 3) g_octave++;
		else if (w == VK_UP) set_mod(g_mod_value + 16);
		else if (w == VK_DOWN) set_mod(g_mod_value - 16);
		else if (w == VK_F9) toggle_record();
		else if (w >= VK_F1 && w <= VK_F7) arp_key(w);
		else if (!(lp & (1 << 30)))
		{
			const int n = key_note(w);
			if (n >= 0) play_on(n, 100);
		}
		return 0;
	case WM_KEYUP:
	{
		const int n = key_note(w);
		if (n >= 0) play_off(n);
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(h, m, w, lp);
}

} // namespace

// Offline check: prophecy_win.exe --render <out.wav> <program> <note> [mod 0-127] [data dir]
// Loads the program, sets the mod wheel, waits 1 s, holds the note 1 s, releases, renders 2 s more.
static int render_offline(const std::string &args)
{
	char wav[MAX_PATH] = {}, dir[MAX_PATH] = {};
	int program = 0, note = 60, mod = 0;
	const int got = std::sscanf(args.c_str(), "--render %259s %d %d %d %259s", wav, &program, &note, &mod, dir);
	if (got < 3)
		return 2;
	if (got == 3)
		mod = 0;
	std::string d = dir;
	if (d.empty())
	{
		char exe[MAX_PATH];
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		d = std::string(exe);
		d = d.substr(0, d.find_last_of("\\/")) + "\\data";
	}
	std::string err;
	const bool loaded = g_synth.load(d, err);
	if (loaded && g_synth.booted())
		g_synth.save_snapshot(d);   // booted natively: the next start loads the snapshot
	if (!loaded)
		return 3;
	std::vector<short> pcm;
	std::vector<float> l(480), r(480);
	std::vector<double> loads;
	const float g = g_gain.load();
	auto run = [&](double seconds) {
		for (int b = 0; b < int(seconds * 100); b++)
		{
			g_synth.render(l.data(), r.data(), 480);
			loads.push_back(g_synth.load_percent());
			for (int k = 0; k < 480; k++)
			{
				pcm.push_back(to_pcm(l[k], g));
				pcm.push_back(to_pcm(r[k], g));
			}
		}
	};
	g_synth.program(program);
	run(0.5);
	g_synth.midi_cc(1, mod);
	run(0.5);
	g_synth.note_on(note, 100);
	run(1.0);
	g_synth.note_off(note);
	run(2.0);
	FILE *f = std::fopen(wav, "wb");
	if (!f)
		return 4;
	const unsigned bytes = unsigned(pcm.size() * 2), rate = 48000, brate = 48000 * 4;
	const unsigned short fmt[] = { 1, 2 }, align[] = { 4, 16 };
	std::fwrite("RIFF", 1, 4, f); const unsigned riff = 36 + bytes; std::fwrite(&riff, 4, 1, f);
	std::fwrite("WAVEfmt ", 1, 8, f); const unsigned sixteen = 16; std::fwrite(&sixteen, 4, 1, f);
	std::fwrite(fmt, 2, 2, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&brate, 4, 1, f); std::fwrite(align, 2, 2, f);
	std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f); std::fwrite(pcm.data(), 2, pcm.size(), f);
	std::fclose(f);
	FILE *log = std::fopen((std::string(wav) + ".txt").c_str(), "w");
	if (log)
	{
		std::vector<double> s = loads;
		std::sort(s.begin(), s.end());
		int over = 0;
		for (double v : s) over += v > 100.0;
		std::fprintf(log, "program %d (%s) note %d, gain %+d dB: block load median %.1f%%, p99 %.1f%%, max %.1f%% (block %d of %zu), blocks over realtime %d\n",
			program, g_synth.program_count() > program ? g_synth.program_name(program).c_str() : "?", note, g_volume_db,
			s[s.size() / 2], s[s.size() * 99 / 100], s.back(),
			int(std::max_element(loads.begin(), loads.end()) - loads.begin()), loads.size(), over);
		std::fclose(log);
	}
	return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int show)
{
	if (cmd && std::strncmp(cmd, "--render", 8) == 0)
		return render_offline(cmd);
	std::string dir = (cmd && *cmd) ? std::string(cmd) : std::string();
	if (!dir.empty() && dir.front() == '"') dir = dir.substr(1, dir.find('"', 1) - 1);
	if (dir.empty())
	{
		char exe[MAX_PATH];
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		dir = std::string(exe);
		dir = dir.substr(0, dir.find_last_of("\\/")) + "\\data";
	}
	std::string err;
	const bool loaded = g_synth.load(dir, err);
	if (loaded && g_synth.booted())
		g_synth.save_snapshot(dir);   // booted natively: the next start loads the snapshot
	if (!loaded)
	{
		MessageBoxA(nullptr, ("Could not load engine data from:\n" + dir + "\n\n" + err).c_str(), "Prophetic", MB_ICONERROR);
		return 1;
	}
	g_synth.load_user_sysex(dir + "\\User");   // your SysEx libraries: programs "Unnn", arpeggio patterns
	for (int i = 0; i < kp::Arp::kPatterns; i++)
		if (const std::uint8_t *b = g_synth.arp_pattern(i))
			g_patterns[size_t(i)] = kp::Arp::PatternData::from_bytes(b);
	select_pattern(g_arp_set, kp::Arp::kUp);
	INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
	InitCommonControlsEx(&icc);
	WNDCLASSA wc{};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.lpszClassName = "KpProphecy";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
	RegisterClassA(&wc);
	g_main = CreateWindowA("KpProphecy", "Prophetic (KP Sim) - test build", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
		CW_USEDEFAULT, CW_USEDEFAULT, 1080, 470, nullptr, nullptr, inst, nullptr);
	ShowWindow(g_main, show);
	std::thread audio(audio_thread);
	HMIDIIN mi = nullptr;
	if (midiInGetNumDevs() > 0 && midiInOpen(&mi, 0, DWORD_PTR(midi_in), 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
		midiInStart(mi);
	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
	g_run = false;
	audio.join();
	if (mi) { midiInStop(mi); midiInClose(mi); }
	return 0;
}
