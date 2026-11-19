// End-to-end checks of the built plugin through its VST2 interface (a minimal host, like the MPC):
// parameter edits change the sound, a saved project restores its edits, a preset change clears
// edits but keeps Volume, OSC Set loads the oscillator-model defaults and the model displays follow,
// the arpeggiator latches, the Performance Editor knobs edit and restore, NRPN switches work.
// Usage: plugin_test <path to the native plugin library>
#include "mpc_vst2_abi.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static void *open_lib(const char *p) { return (void *)LoadLibraryA(p); }
static void *sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
#else
#include <dlfcn.h>
static void *open_lib(const char *p) { return dlopen(p, RTLD_NOW); }
static void *sym(void *h, const char *n) { return dlsym(h, n); }
#endif

namespace {

int fails = 0;
constexpr int kp_max_voices() { return 8; }   // the Voices parameter's range: 1..8 (kp::PolySynth::kMaxVoices)
void check(bool ok, const char *what)
{
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

VstIntPtr host_cb(AEffect *, VstInt32 op, VstInt32, VstIntPtr, void *, float)
{
	return op == 1 /* audioMasterVersion */ ? 2400 : 0;
}

struct Instance
{
	AEffect *e = nullptr;
	std::vector<float> l = std::vector<float>(128), r = std::vector<float>(128);

	explicit Instance(AEffect *(*entry)(audioMasterCallback))
	{
		e = entry(host_cb);
		disp(effOpen);
		disp(effSetSampleRate, 0, 0, nullptr, 44100.0f);
		disp(effSetBlockSize, 0, 128);
		disp(effMainsChanged, 0, 1);
	}
	~Instance() { disp(effMainsChanged, 0, 0); disp(effClose); }
	VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void *p = nullptr, float o = 0) { return e->dispatcher(e, op, idx, v, p, o); }
	int param(const char *name)
	{
		for (int i = 0; i < e->numParams; i++)
		{
			char b[256] = {};
			disp(effGetParamName, i, 0, b);
			if (!std::strcmp(b, name))
				return i;
		}
		std::printf("no parameter %s\n", name);
		return 0;
	}
	std::string display(int i)
	{
		char b[256] = {};
		disp(effGetParamDisplay, i, 0, b);
		return b;
	}
	void midi(int s, int d1, int d2)
	{
		VstMidiEvent m = {};
		m.type = kVstMidiType;
		m.byteSize = sizeof m;
		m.midiData[0] = char(s);
		m.midiData[1] = char(d1);
		m.midiData[2] = char(d2);
		VstEvents ev = {};
		ev.numEvents = 1;
		ev.events[0] = reinterpret_cast<VstEvent *>(&m);
		disp(effProcessEvents, 0, 0, &ev);
	}
	double run(double seconds)   // returns the RMS of the rendered audio
	{
		float *out[2] = { l.data(), r.data() };
		double sum = 0;
		long n = 0;
		for (int b = int(seconds * 44100 / 128); b > 0; b--)
		{
			e->processReplacing(e, nullptr, out, 128);
			for (int i = 0; i < 128; i++) { sum += double(l[i]) * l[i] + double(r[i]) * r[i]; n += 2; }
		}
		return std::sqrt(sum / (n ? n : 1));
	}
	double note(int key)   // hold a note: RMS of 0.3 s after 0.3 s
	{
		midi(0x90, key, 100);
		run(0.3);
		const double v = run(0.3);
		midi(0x80, key, 0);
		run(0.6);
		return v;
	}
	std::vector<char> chunk()
	{
		void *p = nullptr;
		const VstIntPtr n = disp(effGetChunk, 0, 0, &p);
		return std::vector<char>((char *)p, (char *)p + n);
	}
};

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin library>\n", argv[0]); return 2; }
	void *lib = open_lib(argv[1]);
	auto entry = lib ? (AEffect *(*)(audioMasterCallback))sym(lib, "VSTPluginMain") : nullptr;
	if (!entry) { std::fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }

	std::vector<char> saved;
	double stock = 0;
	{
		Instance a(entry);
		std::printf("%d parameters, %d programs\n", a.e->numParams, a.e->numPrograms);
		const int out = a.param("Out Level"), oscset = a.param("OSC Set");
		a.disp(effSetProgram, 0, 0);
		a.run(0.8);
		std::printf("     A00: Out Level %s, OSC Set %s, OSC1 Model %s\n", a.display(out).c_str(), a.display(oscset).c_str(),
		            a.display(a.param("OSC1 Model")).c_str());
		stock = a.note(60);
		a.e->setParameter(a.e, out, 0.0f);   // Out Level 0
		a.run(0.2);
		const double edited = a.note(60);
		std::printf("     note rms: stock %.4f, Out Level 0 %.4f\n", stock, edited);
		check(stock > 1e-3, "program sounds");
		check(edited < 0.05 * stock, "Out Level edit silences it");
		saved = a.chunk();
	}
	{
		Instance b(entry);
		b.disp(effSetChunk, 0, VstIntPtr(saved.size()), saved.data());
		b.run(0.8);
		const int out = b.param("Out Level");
		const double restored = b.note(60);
		std::printf("     restored project: Out Level %s, note rms %.4f\n", b.display(out).c_str(), restored);
		check(b.display(out) == "0", "restored project keeps the edited value");
		check(restored < 0.05 * stock, "restored project sounds edited");

		const int vol = b.param("Volume");
		b.e->setParameter(b.e, vol, 0.25f);   // -12 dB
		const std::string vol_text = b.display(vol);
		b.disp(effSetProgram, 0, 1);
		b.run(0.8);
		std::printf("     program 1: Volume %s (was %s), Out Level %s\n", b.display(vol).c_str(), vol_text.c_str(), b.display(out).c_str());
		check(b.display(vol) == vol_text, "preset change keeps Volume");
		check(b.display(out) != "0", "preset change clears the edit");
		check(b.note(60) > 1e-4, "program 1 sounds");

		const int oscset = b.param("OSC Set"), noise = b.param("O1C Noise"), m1 = b.param("OSC1 Model");
		b.e->setParameter(b.e, oscset, 4.0f / 11.0f);   // set 5: Comb / Comb
		b.run(0.6);
		std::printf("     OSC Set %s: OSC1 Model %s, O1C Noise %s\n", b.display(oscset).c_str(), b.display(m1).c_str(), b.display(noise).c_str());
		check(b.display(m1) == "Comb", "model display follows OSC Set");
		check(b.display(noise) == "99", "OSC1 Comb parameters hold the model defaults");
		check(b.note(64) > 1e-4, "Comb/Comb sounds");
	}
	{
		Instance c(entry);
		c.disp(effSetProgram, 0, 0);
		c.run(0.8);
		const int on = c.param("Arp On"), latch = c.param("Arp Latch");
		// no tails: amp EG release 0, effects dry (A00 rings on for seconds otherwise)
		c.e->setParameter(c.e, c.param("AEG Rel Time"), 0.0f);
		for (const char *fx : { "Dly Bal", "Rev Bal", "Cho Bal", "Dist Bal", "Wah Bal" })
			c.e->setParameter(c.e, c.param(fx), 0.0f);
		c.run(0.3);
		c.midi(0x90, 60, 100);   // without the arpeggiator: release ends the note
		c.run(0.3);
		c.midi(0x80, 60, 0);
		c.run(1.5);
		const double plain_tail = c.run(0.3);
		c.e->setParameter(c.e, on, 1.0f);
		c.e->setParameter(c.e, latch, 1.0f);
		c.run(0.1);
		c.midi(0x90, 60, 100);
		c.midi(0x90, 67, 100);
		c.run(0.3);
		c.midi(0x80, 60, 0);
		c.midi(0x80, 67, 0);
		c.run(1.5);
		const double latched = c.run(0.3);
		std::printf("     arp latch: rms 1.5 s after release %.4f (no arp %.4f)\n", latched, plain_tail);
		check(latched > 4 * plain_tail + 1e-4, "latched arpeggio keeps playing after release");
		c.e->setParameter(c.e, latch, 0.0f);
		c.run(2.0);
		const double unlatched = c.run(0.3);
		check(unlatched < 0.5 * latched, "releasing latch stops the arpeggio");
		// a user pattern brings its own settings (PAT1: velocity and gate per step) and plays
		const int pat = c.param("Arp Pattern"), vel = c.param("Arp Velocity"), gate = c.param("Arp Gate");
		c.e->setParameter(c.e, pat, 5.0f / 9.0f);   // Pat1
		c.run(0.1);
		std::printf("     Arp Pattern %s: Velocity %s, Gate %s, Step %s\n", c.display(pat).c_str(), c.display(vel).c_str(),
		            c.display(gate).c_str(), c.display(c.param("Arp Step")).c_str());
		check(c.display(pat) == "Pat1" && c.display(vel) == "Step" && c.display(gate) == "Step", "Pat1 loads its settings (Step velocity / gate)");
		c.midi(0x90, 60, 100);
		c.midi(0x90, 64, 100);
		c.midi(0x90, 67, 100);
		const double pat1 = c.run(1.0);
		c.midi(0x80, 60, 0); c.midi(0x80, 64, 0); c.midi(0x80, 67, 0);
		c.run(0.5);
		check(pat1 > 1e-3, "Pat1 arpeggio plays");
	}
	{
		// Performance Editor: Perf Knob 1 on A00 edits the parameters its PE sets are assigned to,
		// the edits show in those parameters, a project keeps them, CC19 is the same knob, PE switches
		// gate it, NRPN switches PE sets and the arpeggiator
		Instance d(entry);
		d.disp(effSetProgram, 0, 0);
		d.run(0.8);
		const int k1 = d.param("Perf Knob 1"), pe[4] = { d.param("PE1 On"), d.param("PE2 On"), d.param("PE3 On"), d.param("PE4 On") };
		const int first = d.param("Hold"), last = d.param("Porta SW");   // the program parameters
		auto snapshot = [&](Instance &x) {
			std::vector<std::string> v;
			for (int i = first; i < last; i++) v.push_back(x.display(i));
			return v;
		};
		const auto at0 = snapshot(d);
		std::printf("     A00: Perf Knob 1 %s, PE1-4 %s %s %s %s\n", d.display(k1).c_str(), d.display(pe[0]).c_str(),
		            d.display(pe[1]).c_str(), d.display(pe[2]).c_str(), d.display(pe[3]).c_str());
		d.e->setParameter(d.e, k1, 1.0f);   // knob 1 to 127
		d.run(0.3);
		const auto at127 = snapshot(d);
		int changed = 0;
		for (size_t i = 0; i < at0.size(); i++)
			if (at0[i] != at127[i])
			{
				char n[256] = {};
				d.disp(effGetParamName, first + int(i), 0, n);
				std::printf("     knob 1 -> 127: %s %s -> %s\n", n, at0[i].c_str(), at127[i].c_str());
				changed++;
			}
		check(changed > 0, "Perf Knob 1 edits its assigned parameters, and they show it");
		auto saved_pe = d.chunk();
		{
			Instance r(entry);
			r.disp(effSetChunk, 0, VstIntPtr(saved_pe.size()), saved_pe.data());
			r.run(0.8);
			check(r.display(k1) == "127" && snapshot(r) == at127, "a project restores the knob and its edits");
		}
		d.midi(0xB0, 19, 0);   // CC19 = Perf Knob 1
		d.run(0.3);
		check(d.display(k1) == "0", "CC19 moves Perf Knob 1");
		const auto back = snapshot(d);   // the knob-0 values (not necessarily the stored ones)
		check(back != at127, "knob 1 back to 0 edits again");
		for (int s = 0; s < 4; s++) d.e->setParameter(d.e, pe[s], 0.0f);
		d.run(0.1);
		d.midi(0xB0, 19, 127);
		d.run(0.3);
		check(snapshot(d) == back, "with PE1-4 off the knob edits nothing");
		d.midi(0xB0, 99, 0); d.midi(0xB0, 98, 6); d.midi(0xB0, 6, 127);   // NRPN 00 06: PE1 on
		d.run(0.1);
		check(d.display(pe[0]) == "On" && d.display(pe[1]) == "Off", "NRPN PE1 On");
		const int arp = d.param("Arp On");
		d.midi(0xB0, 98, 2); d.midi(0xB0, 6, 127);   // NRPN 00 02: arpeggiator on
		d.run(0.1);
		check(d.display(arp) == "On", "NRPN Arpeggio On");
		d.midi(0xB0, 97, 0);   // data decrement: off
		d.run(0.1);
		check(d.display(arp) == "Off", "NRPN data decrement turns it off");
		const int kc1 = d.param("Knob1 Ctrl");
		d.e->setParameter(d.e, kc1, 25.0f / 100.0f);   // CC#20 (Off, 4 bend/AT choices, then CC#0..95)
		d.run(0.1);
		d.midi(0xB0, 20, 100);
		d.run(0.2);
		std::printf("     Knob1 Ctrl %s, after CC20 100: Perf Knob 1 %s\n", d.display(kc1).c_str(), d.display(k1).c_str());
		check(d.display(kc1) == "CC#20" && d.display(k1) == "100", "Knob1 Ctrl CC#20: CC20 turns Perf Knob 1");
		d.e->setParameter(d.e, kc1, 24.0f / 100.0f);   // back to CC#19
		d.run(0.1);
	}
	{
		// Define editors: PE Param of set 1 / knob 1 redirected to F1 Cutoff takes effect on the next
		// knob move; a pattern step edit; a project keeps both
		Instance d(entry);
		d.disp(effSetProgram, 0, 0);
		d.run(0.8);
		const int k1 = d.param("Perf Knob 1"), pp = d.param("PE Param"), cut = d.param("F1 Cutoff"), wf = d.param("O1S WaveForm");
		d.e->setParameter(d.e, d.param("PE Ed Set"), 0.0f);
		d.e->setParameter(d.e, d.param("PE Ed Knob"), 0.0f);
		d.run(0.1);
		const std::string stored = d.display(pp);
		d.e->setParameter(d.e, pp, 28.0f / 255.0f);   // PE #28
		d.run(0.1);
		const std::string cut0 = d.display(cut), wf0 = d.display(wf);
		d.e->setParameter(d.e, k1, 1.0f);
		d.run(0.3);
		std::printf("     PE1 knob 1: %s -> %s; knob to 127: F1 Cutoff %s -> %s, O1S WaveForm %s -> %s\n", stored.c_str(),
		            d.display(pp).c_str(), cut0.c_str(), d.display(cut).c_str(), wf0.c_str(), d.display(wf).c_str());
		check(d.display(pp) == "F1 Cutoff" && d.display(cut) != cut0, "PE Define: knob 1 now edits F1 Cutoff");
		const int pat = d.param("Arp Pattern"), step = d.param("Pat Ed Step"), pv = d.param("Pat Velocity");
		d.e->setParameter(d.e, pat, 5.0f / 9.0f);   // Pat1
		d.run(0.1);
		d.e->setParameter(d.e, step, 2.0f / 23.0f); // step 3
		d.run(0.1);
		d.e->setParameter(d.e, pv, 32.0f / 126.0f); // velocity 33
		d.run(0.1);
		check(d.display(pv) == "33", "Pat1 step 3 velocity edited");
		auto saved = d.chunk();
		Instance r(entry);
		r.disp(effSetChunk, 0, VstIntPtr(saved.size()), saved.data());
		r.run(0.8);
		r.e->setParameter(r.e, r.param("PE Ed Knob"), 1.0f / 4.0f);   // look elsewhere, then back
		r.run(0.1);
		r.e->setParameter(r.e, r.param("PE Ed Knob"), 0.0f);
		r.e->setParameter(r.e, r.param("Pat Ed Step"), 0.0f);
		r.run(0.1);
		r.e->setParameter(r.e, r.param("Pat Ed Step"), 2.0f / 23.0f);
		r.run(0.1);
		std::printf("     restored: PE1 knob 1 %s, Pat1 step 3 velocity %s\n", r.display(r.param("PE Param")).c_str(), r.display(r.param("Pat Velocity")).c_str());
		check(r.display(r.param("PE Param")) == "F1 Cutoff" && r.display(r.param("Pat Velocity")) == "33",
		      "a project keeps the PE Define and pattern edits");
		// polyphonic aftertouch on the held key turns a knob assigned to aftertouch (Knob1 Ctrl = ATouch)
		r.e->setParameter(r.e, r.param("Knob1 Ctrl"), 4.0f / 100.0f);
		r.run(0.1);
		r.midi(0x90, 60, 100);
		r.midi(0xA0, 60, 127);
		r.run(0.2);
		std::printf("     poly aftertouch 127 on the held key: Perf Knob 1 %s\n", r.display(r.param("Perf Knob 1")).c_str());
		check(r.display(r.param("Perf Knob 1")) == "31", "poly aftertouch acts as the Prophecy's aftertouch (knob on ATouch: first ramp step 31)");
		r.midi(0x80, 60, 0);
		r.midi(0xB0, 74, 33);
		r.run(0.05);
		std::printf("     MIDI In 1..3: %s | %s | %s\n", r.display(r.param("MIDI In 1")).c_str(), r.display(r.param("MIDI In 2")).c_str(),
		            r.display(r.param("MIDI In 3")).c_str());
		check(r.display(r.param("MIDI In 1")) == "B0 4A 21" && r.display(r.param("MIDI In 2")) == "80 3C 00", "MIDI In shows the last messages, newest first");
		// MIDI Log: a background thread appends what arrives to Content/midi_log.txt
		r.e->setParameter(r.e, r.param("MIDI Log"), 1.0f);
		r.run(0.02);
		r.midi(0xB9, 0x18, 0x42);
		r.run(0.02);
		r.midi(0x90, 60, 100);
		r.run(1.2);   // a full second of audio: a LOAD line
		r.midi(0x80, 60, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		r.e->setParameter(r.e, r.param("MIDI Log"), 0.0f);
		r.run(0.02);
		bool logged = false, load_line = false;
		for (const char *dir : { "Content", "../Content", "../../Content" })
		{
			std::ifstream f(std::string(dir) + "/midi_log.txt");
			std::string line, last, load;
			while (std::getline(f, line))
			{
				if (line.find("B9 18 42") != std::string::npos) last = line;
				if (line.find(" LOAD avg ") != std::string::npos) load = line;
			}
			if (!last.empty())
			{
				std::printf("     %s/midi_log.txt: %s | %s\n", dir, last.c_str(), load.c_str());
				logged = true;
				load_line = !load.empty();
				break;
			}
		}
		check(logged, "MIDI Log writes the received messages to Content/midi_log.txt");
		check(load_line, "MIDI Log writes the engine load once a second (LOAD lines)");
		char mpe[] = "MPE";
		check(r.disp(effCanDo, 0, 0, mpe) == 1, "the plugin tells the host it takes MPE");
		r.midi(0xB0, 120, 0);   // All Sound Off
		r.run(0.1);
	}
	{
		// Stop on the MPC: the host suspends and resumes the plugin (no MIDI). A held note on the
		// program that keeps sounding longest after All Sound Off goes quiet; the next note plays.
		Instance s(entry);
		s.disp(effSetProgram, 0, 0);
		s.run(0.8);
		const int load = s.param("Engine Load");
		s.midi(0x90, 60, 110);
		const double held = s.run(1.5);
		const float load_playing = s.e->getParameter(s.e, load);
		s.disp(effMainsChanged, 0, 0);
		s.disp(effMainsChanged, 0, 1);
		s.run(0.05);
		const double after = s.run(0.3);
		s.run(2.5);   // the program keeps decaying for ~11 s behind the fade; the engine sleeps 1 s after it
		const float load_stopped = s.e->getParameter(s.e, load);
		std::printf("     Stop: Engine Load playing %.2f%%, 2.8 s after Stop %.2f%%\n", 100.0 * load_playing, 100.0 * load_stopped);
		check(load_stopped < 0.25f * load_playing, "after Stop the engine sleeps (Engine Load drops) although the program still decays");
		s.midi(0x90, 64, 110);
		s.run(0.05);
		const double next = s.run(0.3);
		s.midi(0x80, 64, 0);
		std::printf("     Stop (suspend / resume): held note rms %.4f, after %.6f, next note %.4f\n", held, after, next);
		check(held > 1e-3 && after < 1e-6 && next > 1e-3, "host reset (MPC Stop) silences a held note; the next note sounds");
	}
	{
		// Voices: the voices switched on take the program's edits (Out Level 0 stays silent on all of
		// them); then a chord on 3 voices sounds as three notes
		Instance v(entry);
		const int voices = v.param("Voices"), out = v.param("Out Level");
		v.disp(effSetProgram, 0, 21);
		v.run(0.8);
		auto chord = [&](std::initializer_list<int> notes) {
			for (int n : notes) v.midi(0x90, n, 100);
			v.run(0.3);
			const double r = v.run(0.3);
			for (int n : notes) v.midi(0x80, n, 0);
			v.run(1.0);
			return r;
		};
		const double one = chord({ 60 });
		const float out_level = v.e->getParameter(v.e, out);
		v.e->setParameter(v.e, out, 0.0f);   // an edit while one voice plays
		v.run(0.2);
		v.e->setParameter(v.e, voices, (3 - 1) / float(kp_max_voices() - 1));
		const std::string shown = v.display(voices);
		v.run(1.0);   // the new voices take the current state
		const double edited = chord({ 60, 64, 67 });
		v.e->setParameter(v.e, out, out_level);
		v.run(0.3);
		const double three = chord({ 60, 64, 67 });
		std::printf("     Voices %s: one note rms %.4f; chord with Out Level 0 %.6f; chord %.4f\n", shown.c_str(), one, edited, three);
		check(shown == "3" && edited < 0.05 * one, "Voices 1 -> 3: the voices switched on take the program's edits");
		check(three > 1.4 * one, "Voices 3: a three-note chord sounds as three notes");
	}
	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
