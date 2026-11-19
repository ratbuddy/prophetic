#!/usr/bin/env python3
"""Generate plugins/Prophecy/ui.json: 32 pages (the MPC maximum) of Prophecy parameters in blocks of
four, following the signal flow. Parameter names come from tools/gen_params.py. Oscillator-model
controls are only visible while that model is active in the slot (OSC1 Model / OSC2 Model, which
the plugin derives from OSC Set). Parameters not placed here are still saved and automatable.

Usage: py tools/gen_prophecy_ui.py [out.json]
"""
from __future__ import annotations

import json
import os
import runpy
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "plugins" / "Prophecy" / "ui.json"

sys.argv = ["gen_params.py", os.path.join(tempfile.gettempdir(), "kp_params_ui.inc")]
g = runpy.run_path(str(REPO / "tools" / "gen_params.py"))
PARAMS = {p["name"]: p for p in g["params"]}
LISTS = g["LISTS"]
EXTRA = {"Volume": ("float", 0, 1), "Engine Load": ("float", 0, 1), "Porta SW": ("bool", 0, 1),
         "Transpose": ("int", -12, 12), "OSC1 Model": ("choice", 0, 7), "OSC2 Model": ("choice", 0, 7),
         "Arp On": ("bool", 0, 1), "Arp Pattern": ("choice", 0, 9), "Arp Octave": ("int", 1, 4),
         "Arp Latch": ("bool", 0, 1), "Arp KeySync": ("bool", 0, 1), "Arp Step": ("choice", 0, 5),
         "Arp Gate": ("int", 1, 101), "Arp Velocity": ("int", 0, 128), "Arp Sort": ("bool", 0, 1),
         "Arp Zone Lo": ("int", 0, 127), "Arp Zone Hi": ("int", 0, 127), "Arp BPM": ("int", 40, 300),
         **{f"Perf Knob {k}": ("int", 0, 127) for k in range(1, 6)},
         **{f"PE{s} On": ("bool", 0, 1) for s in range(1, 5)},
         "Master Tune": ("float", 430, 450), "Dly/Rev On": ("bool", 0, 1),
         "Arp Vel Int": ("int", -99, 99), "Arp Gate Int": ("int", -99, 99),
         "PE Ed Set": ("int", 1, 4), "PE Ed Knob": ("int", 1, 5), "PE Param": ("choice", 0, 255),
         "PE Left": ("int", 0, 100), "PE Right": ("int", 0, 100), "PE Curve": ("choice", 0, 2),
         "Pat Ed Step": ("int", 1, 24), "Pat Tone": ("int", 1, 13), "Pat Velocity": ("int", 1, 127),
         "Pat Gate": ("int", 0, 100), "Pat Offset": ("int", -49, 49), "Arp Type": ("choice", 0, 3),
         "Arp Oct Alt": ("choice", 0, 2),
         "MIDI In 1": ("int", 0, 999), "MIDI In 2": ("int", 0, 999), "MIDI In 3": ("int", 0, 999), "MIDI Log": ("bool", 0, 1), "Idle Sleep": ("bool", 0, 1),
         "Voices": ("int", 1, 8),
         **{f"User1 {n}": ("int", -100, 100) for n in ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]}}


def widget(name: str) -> dict:
    if name == "empty":
        return {"widget": "empty"}
    if name in EXTRA:
        kind = EXTRA[name][0]
        if kind == "bool":
            return {"param": name, "widget": "switch"}
        if name == "Volume":
            return {"param": name, "widget": "fader"}
        if kind == "choice":
            n = EXTRA[name][2] - EXTRA[name][1] + 1
            return {"param": name, "widget": "switch"} if n <= 4 else {"param": name, "widget": "slider", "overlay": "menu"}
        if name in ("Arp Zone Lo", "Arp Zone Hi"):
            return {"param": name, "widget": "slider"}
        return {"param": name, "widget": "knob"}
    p = PARAMS[name]
    f = p["fmt"]
    if f.startswith("C:") or f in ("LFO", "EG"):
        n = len(LISTS[f[2:] if f.startswith("C:") else f])
        return {"param": name, "widget": "switch"} if n <= 4 else {"param": name, "widget": "slider", "overlay": "menu"}
    if f == "NOTE" or f == "OCT":
        return {"param": name, "widget": "slider"}
    if p["lo"] < 0:
        return {"param": name, "widget": "knob", "polarity": "bipolar"}
    level_words = ("Level", "Lvl", "Bal", "Time", "Sus", "Atk", "Dcy", "Rel", "Slp", "Start", "Break")
    if any(w in name for w in level_words) and p["hi"] <= 127:
        return {"param": name, "widget": "fader"}
    return {"param": name, "widget": "knob"}


def eg_page(n):
    e = f"EG{n}"
    return [f"{e} Start", f"{e} Atk Time", f"{e} Atk Level", f"{e} Dcy Time",
            f"{e} Break Pt", f"{e} Slp Time", f"{e} Sus Level", f"{e} Rel Time",
            f"{e} Rel Level", f"{e} KT Atk", f"{e} KT Dcy", f"{e} KT Rel",
            f"{e} Vel Level", f"{e} Vel Atk", f"{e} Vel Dcy", f"{e} Vel Rel"]


def lfo_page(n):
    s = f"LFO{n}"
    return [f"{s} Wave", f"{s} Freq", f"{s} Mode", f"{s} KeySync",
            f"{s} FrqKT", f"{s} FrqCC1", f"{s} FrqSrc", f"{s} FrqInt",
            f"{s} Offset", f"{s} AmpSrc", f"{s} AmpInt", "empty",
            f"{s} Delay", f"{s} FadeIn", "empty", "empty"]


def model_half(prefix, names, slot, model):
    out = []
    for n in names:
        c = widget(f"{prefix} {n}") if n != "empty" else {"widget": "empty"}
        if n != "empty":
            c["visibleWhen"] = {"param": f"OSC{slot} Model", "choice": model}
        out.append(c)
    return out


def model_page(prefix, names, slot, model):
    return model_half(prefix, names, slot, model)


PAGES = [
    # the Prophecy's performance controls: the five Performance Editor knobs and the PE1-4 switches
    ("MAIN", ["Volume", "Out Level", "Porta SW", "Transpose",
              "Perf Knob 1", "Perf Knob 2", "Perf Knob 3", "Perf Knob 4",
              "Perf Knob 5", "OSC Set", "Idle Sleep", "Voices",
              "PE1 On", "PE2 On", "PE3 On", "PE4 On"]),
    ("OSC", ["O1 Octave", "O1 Semi", "O1 Fine", "O1 FreqOfs",
             "O2 Octave", "O2 Semi", "O2 Fine", "O2 FreqOfs",
             "O1 P.LFO", "O1 P.LFOInt", "O1 P.ModSrc", "O1 P.ModInt",
             "O2 P.LFO", "O2 P.LFOInt", "O2 P.ModSrc", "O2 P.ModInt"]),
    ("OSC KT", ["O1 SlpLoKey", "O1 SlpHiKey", "O1 LoSlope", "O1 HiSlope",
                "O2 SlpLoKey", "O2 SlpHiKey", "O2 LoSlope", "O2 HiSlope",
                "O1 P.LFO AT", "O1 P.LFOCC1", "O2 P.LFO AT", "O2 P.LFOCC1",
                "Sub Source", "Sub Semi", "Sub Fine", "Sub Wave"]),
    ("PITCH EG", ["PEG Start", "PEG Atk Time", "PEG Atk Level", "PEG Dcy Time",
                  "PEG Break Pt", "PEG Slp Time", "PEG Rel Time", "PEG Rel Level",
                  "PEG KT Lvl", "PEG KT Time", "PEG Vel Lvl", "PEG Vel Tim",
                  "Bend +X", "Bend -X", "Bend Step+", "Bend Step-"]),
    ("VOICE", ["AT Bend", "Porta Mode", "Porta Time", "Porta VelCtl",
               "Key Prio", "Trigger", "Trig Vel", "Trig Above",
               "Scale Key", "Scale Type", "Rand Pitch", "F Routing",
               "Noise LPF", "Noise LPF KT", "OSC1 Model", "OSC2 Model"]),
]
MODEL_PAGES = [
    ("STD", model_half("O1S", ["Wave", "Edge", "WaveLvl", "RampLvl", "WaveForm", "WF LFO", "WF LFOInt", "WF Int"], 1, "Std")
     + model_half("O2S", ["Wave", "Edge", "WaveLvl", "RampLvl", "WaveForm", "WF LFO", "WF LFOInt", "WF Int"], 2, "Std")),
    ("COMB", model_half("O1C", ["Noise", "InWave", "InWvLvl", "InGain", "Feedback", "FB EG", "FB EGInt", "LoopLPF"], 1, "Comb")
     + model_half("O2C", ["Noise", "InWave", "InWvLvl", "InGain", "Feedback", "FB EG", "FB EGInt", "LoopLPF"], 2, "Comb")),
    ("MOD", model_half("O1M", ["Type", "Input", "RingCar", "XCar", "XDepth", "XDepEGInt", "SyncWave", "SyncShape"], 1, "Mod")
     + model_half("O2M", ["Type", "Input", "RingCar", "XCar", "XDepth", "XDepEGInt", "SyncWave", "SyncShape"], 2, "Mod")),
    ("O1 VPM", model_page("O1V", ["CarWave", "CarLvl", "CarEGInt", "CarInt", "TblShape", "TblLFOInt", "TblInt", "FB Gain",
                                  "ModSemi", "ModFine", "ModPchKT", "ModPInt", "ModWave", "ModLvl", "ModEGInt", "ModLInt"], 1, "VPM")),
    ("O2 VPM", model_page("O2V", ["CarWave", "CarLvl", "CarEGInt", "CarInt", "TblShape", "TblLFOInt", "TblInt", "FB Gain",
                                  "ModSemi", "ModFine", "ModPchKT", "ModPInt", "ModWave", "ModLvl", "ModEGInt", "ModLInt"], 2, "VPM")),
    ("BRASS", model_page("O1B", ["Instr", "BendCtl", "BendInt", "BendDir", "PrsEG", "PrsEGInt", "PrsLFO", "PrsLFOInt",
                                 "PrsSrc", "PrsInt", "Lip", "LipInt", "Bell", "BellTone", "BellReso", "Noise"], 1, "Brass")),
    ("REED", model_page("O1R", ["Instr", "BendCtl", "BendInt", "BendDir", "PrsEG", "PrsEGInt", "PrsEGSrc", "PrsEGMod",
                                "PrsLFO", "PrsLFOInt", "PrsSrc", "PrsInt", "ReedSrc", "ReedInt", "Noise", "empty"], 1, "Reed")),
    ("PLUCK", model_page("O1P", ["AtkLvl", "AtkLvlVel", "AtkNoise", "AtkNzVel", "NzFilter", "NzCutoff", "NzReso", "AtkEdge",
                                 "StrPos", "StrPosVel", "StrLoss", "StrLossKT", "Inharm", "InharmKT", "Decay", "Release"], 1, "Pluck")),
]
PAGES2 = [
    # both wave shapers on one page (their modulation source / intensity are automation-only)
    ("SHAPE", ["WS1 InGain", "WS1 InOffs", "WS1 Table", "WS1 Shape",
               "WS1 Fdback", "WS1 XLoop", "WS1 OutGain", "WS1 Thru",
               "WS2 InGain", "WS2 InOffs", "WS2 Table", "WS2 Shape",
               "WS2 Fdback", "WS2 XLoop", "WS2 OutGain", "WS2 Thru"]),
    # the Prophecy's Define modes: pick a PE set / knob, edit it; pick a step of the selected pattern, edit it
    ("DEFINE", ["PE Ed Set", "PE Ed Knob", "PE Param", "PE Curve",
                "PE Left", "PE Right", "Arp Type", "Arp Oct Alt",
                "Pat Ed Step", "Pat Tone", "Pat Velocity", "Pat Gate",
                "Pat Offset", "MIDI In 1", "MIDI In 2", "MIDI Log"]),
    ("MIXER", ["Mx O1>1 Lvl", "Mx O2>1 Lvl", "Mx Sub>1 Lvl", "Mx Nz>1 Lvl",
               "Mx O1>2 Lvl", "Mx O2>2 Lvl", "Mx Sub>2 Lvl", "Mx Nz>2 Lvl",
               "Mx FB>1 Lvl", "Mx FB>2 Lvl", "Engine Load", "empty",
               "Mx O1>1 Src", "Mx O1>1 Int", "Mx O2>1 Src", "Mx O2>1 Int"]),
    # arpeggiator, then the global Master Tune and Delay/Reverb switch (User Scale 1 is automation-only)
    ("ARP/GLB", ["Arp On", "Arp Pattern", "Arp Octave", "Arp Step",
                 "Arp Gate", "Arp Velocity", "Arp Latch", "Arp KeySync",
                 "Arp Sort", "Arp Zone Lo", "Arp Zone Hi", "Arp BPM",
                 "Master Tune", "Dly/Rev On", "Arp Vel Int", "Arp Gate Int"]),
    ("FILTER1", ["F1 Type", "F1 Cutoff", "F1 Reso", "F1 InTrim",
                 "F1 EG", "F1 EG Int", "F1 LFO", "F1 LFO Int",
                 "F1 ModSrc", "F1 ModInt", "F1 ResoSrc", "F1 ResoInt",
                 "F1 KT LoKey", "F1 KT HiKey", "F1 KT LoInt", "F1 KT HiInt"]),
    ("FILTER2", ["F2 Type", "F2 Cutoff", "F2 Reso", "F2 InTrim",
                 "F2 EG", "F2 EG Int", "F2 LFO", "F2 LFO Int",
                 "F2 ModSrc", "F2 ModInt", "F2 ResoSrc", "F2 ResoInt",
                 "F2 KT LoKey", "F2 KT HiKey", "F2 KT LoInt", "F2 KT HiInt"]),
    ("AMP", ["A1 Level", "A1 EG", "A1 EG Int", "A1 ModInt",
             "A2 Level", "A2 EG", "A2 EG Int", "A2 ModInt",
             "A1 KT LoInt", "A1 KT HiInt", "A1 ModSrc", "empty",
             "A2 KT LoInt", "A2 KT HiInt", "A2 ModSrc", "empty"]),
    ("AMP EG", ["AEG Start", "AEG Atk Time", "AEG Atk Level", "AEG Dcy Time",
                "AEG Break Pt", "AEG Slp Time", "AEG Sus Level", "AEG Rel Time",
                "AEG KT Atk", "AEG KT Dcy", "AEG KT Slope", "AEG KT Rel",
                "AEG Vel Level", "AEG Vel Atk", "AEG Vel Dcy", "AEG Vel Rel"]),
]
PAGES3 = [(f"EG{n}", eg_page(n)) for n in range(1, 5)] + [(f"LFO{n}", lfo_page(n)) for n in range(1, 5)] + [
    ("DIST/WAH", ["Dist Gain", "Dist Tone", "Dist Level", "Dist Bal",
                  "Wah Reso", "Wah LoFreq", "Wah HiFreq", "Wah SwpDir",
                  "Wah SwpSrc", "Wah Level", "Wah Bal", "Dist BalSrc",
                  "Dist BalInt", "Wah BalSrc", "Wah BalInt", "empty"]),
    ("CHO/DLY", ["DlyRev Sel", "Cho Time", "Cho Fdback", "Cho Depth",
                 "Cho LFO", "Cho DepSrc", "Cho DepInt", "Cho Bal",
                 "Dly Time", "Dly Fdback", "Dly HiDamp", "Dly Bal",
                 "Cho BalSrc", "Cho BalInt", "Dly BalSrc", "Dly BalInt"]),
    ("REV/EQ", ["Rev PreDly", "Rev Time", "Rev HiDamp", "Rev Bal",
                "EQ Lo Freq", "EQ Lo Q", "EQ Lo Gain", "Rev BalSrc",
                "EQ Hi Freq", "EQ Hi Q", "EQ Hi Gain", "Rev BalInt",
                "Pan Src", "Pan Int", "Out Level", "Pan"]),
]

pages = []
for name, controls in PAGES + PAGES2[:0]:
    pages.append({"name": name, "controls": [widget(c) for c in controls]})
for name, controls in MODEL_PAGES:
    pages.append({"name": name, "controls": controls})
for name, controls in PAGES2 + PAGES3:
    pages.append({"name": name, "controls": [widget(c) for c in controls]})
assert len(pages) <= 32, len(pages)
for p in pages:
    assert len(p["controls"]) == 16, (p["name"], len(p["controls"]))
    assert len(p["name"]) <= 8, p["name"]
placed = {c["param"] for p in pages for c in p["controls"] if "param" in c}
unknown = [n for n in placed if n not in PARAMS and n not in EXTRA]
assert not unknown, unknown
missing = sorted(n for n in PARAMS if n not in placed)
OUT.write_text(json.dumps({"pages": pages}, indent=1) + "\n", encoding="utf-8", newline="\n")
print(f"wrote {OUT}: {len(pages)} pages, {len(placed)} parameters on screen, {len(missing)} automation-only")
print("automation-only:", ", ".join(missing))
