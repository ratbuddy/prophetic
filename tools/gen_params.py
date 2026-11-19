#!/usr/bin/env python3
"""Generate engine/src/control/params_table.inc: the Prophecy program parameters.

Source: Korg Prophecy Parameter Guide, 6.6 MIDI Implementation ("Program Parameters", the
PARAMETER CHANGE message and the program dump layout) - public documentation, no firmware data.
Facts verified on the oracle (reference/wsl/param_sweep.py):
  - the V55 forwards a PARAMETER CHANGE to the H8 as `40 hh ll vv`: hh = ExID << 4 | id >> 8,
    ll = id & 0xff, vv = the raw value byte (signed values two's complement)
  - oscillator-model parameters (ExID 1 = OSC1, 2 = OSC2) are forwarded only for the model active
    in that slot; OSC Set (154) makes the V55 re-send the whole program (record + controllers)
  - the H8 program record is the dump layout's first 435 bytes (everything before Controller
    Assignment and Performance Editor), so offsets below are dump offsets. Every offset and
    bitfield below equals the H8 firmware's own parameter table (korgpro20: 10-byte entries at
    0x0EB0A + 10 * id = {u16 offset, u16 (shift << 8 | width - 1), u32 handler, u16}; model
    parameters relative to the oscillator block at 158 / 206); checked with the user's ROM.

Usage: py tools/gen_params.py [out.inc]
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "engine" / "src" / "control" / "params_table.inc"

# ---- value formats -----------------------------------------------------------------------------
# fmt: N number, S signed number (same as N, shown with sign), C:<list> choice (raw = index + base),
# SRC modulation source (0..111), LFO (raw 7..10), EG (raw 1..6), NOTE, OCT, HZ (raw/10 Hz),
# SLOPE (raw*0.02), PCT, DB, CENT, BSTEP (bender step), LFOWAVE, CTL? (not used)
LISTS = {
    "OFFON": ["Off", "On"],
    "PRIO": ["Low", "High", "Last"],
    "TRIG": ["Multi", "Single", "Velocity"],
    "ABOVE": ["Above", "Below"],
    "KEY12": ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"],
    "LFOMODE": ["On", "Off", "Both"],
    "OSCSET": ["1 Std/Std", "2 Std/Comb", "3 Std/VPM", "4 Std/Mod", "5 Comb/Comb", "6 Comb/VPM",
               "7 Comb/Mod", "8 VPM/VPM", "9 VPM/Mod", "10 Brass", "11 Reed", "12 Pluck"],
    "FINGER": ["Normal", "Fingered"],
    "SUBSRC": ["OSC1", "OSC2"],
    "SUBWAVE": ["Sin", "Saw", "Squ", "Tri"],
    "SHAPETBL": ["Clip", "Reso"],
    "ROUTING": ["Serial1", "Serial2", "Parallel"],
    "FTYPE": ["Thru", "LPF", "HPF", "BPF", "BRF"],
    "SWEEPDIR": ["+", "-"],
    "DLYSEL": ["Cho&Delay", "Reverb"],
    "STDWAVE": ["Saw", "Pulse"],
    "COMBIN": ["Saw", "Squ", "Tri"],
    "VPMCAR": ["Sin", "Saw", "Tri", "Squ"],
    "VPMMOD": ["Sin", "Saw", "Tri", "Squ", "OSC"],
    "MODTYPE": ["Ring", "Cross", "Sync"],
    "MODIN": ["OSC1", "Feedback", "Noise"],
    "RINGCAR": ["Sin", "Saw", "Squ"],
    "SYNCWAVE": ["Saw", "Tri"],
    "BRASSTYPE": ["Trumpet1", "Trumpet2", "Trombone", "Horn"],
    "REEDTYPE": ["SoprSax", "AltoSax1", "AltoSax2", "TenrSax1", "TenrSax2", "BariSax", "Flute",
                 "SingReed", "DblReed", "Recorder", "Bottle", "GlassBtl", "Monster"],
    "SBDIR": ["Up", "Down", "Both"],
    "BELL": ["Open", "Mute"],
    "PNFILT": ["LPF", "HPF", "BPF"],
    "LFOWAVE": ["Sin 0", "Sin 180", "Cos 0", "Cos 180", "Tri 0", "Tri 90", "Tri 180", "Tri 270",
                "SawUp 0", "SawUp180", "SawDn 0", "SawDn180", "Sqr 0", "Sqr 180", "Random1",
                "Random2", "Random3", "Random4", "Random5", "Random6", "Growl", "GtrVib", "StepTri",
                "StepSaw", "StepTri4", "StepSaw6", "ExpSawUp", "ExpSawDn", "ExpTri", "StrVib"],
    "SRC": ["Off", "EG1", "EG2", "EG3", "EG4", "PitchEG", "AmpEG", "LFO1", "LFO2", "LFO3", "LFO4",
            "Portamnt", "Note", "Velocity", "PBend", "AftTouch"] + [f"CC#{n}" for n in range(96)],
    "LFO": ["LFO1", "LFO2", "LFO3", "LFO4"],
    "EG": ["EG1", "EG2", "EG3", "EG4", "PitchEG", "AmpEG"],
    "BSTEP": ["Contin", "1/8", "1/4", "1/2"] + [str(n) for n in range(1, 13)],
    "SCALE": ["EqualTmp", "PureMaj", "PureMin", "Arabic", "Pythagor", "Werckmst", "Kirnbrgr", "Slendro",
              "Pelog", "User1", "User2"],
}

# model numbers (slot models): 1 Std, 2 Comb, 3 VPM, 4 Mod, 5 Brass, 6 Reed, 7 Pluck
MODELS = ["", "Std", "Comb", "VPM", "Mod", "Brass", "Reed", "Pluck"]

params: list[dict] = []
_cur = {"off": 0, "byte": None}


def P(pid, name, lo, hi, fmt="N", size="1", section="", key=None, offset=None, model=0, slot=0, exid=0,
      expose=True):
    """size: '1' new byte, 'Bx-y' new byte bits x..y, '^x-y' same byte bits x..y, 'R' reserved byte,
    '<n>' n-byte block (not exposed)."""
    if offset is None:
        if size.startswith("^"):
            off = _cur["byte"]
        else:
            off = _cur["off"]
            _cur["byte"] = off
            _cur["off"] += int(size) if size.isdigit() else 1
    else:
        off = offset
    if size in ("1", "R") or size.isdigit():
        shift, bits = 0, 8
    else:
        a, b = size[1:].split("-") if "-" in size else (size[1:], size[1:])
        shift, bits = int(a), int(b) - int(a) + 1
    if size == "R" or not expose:
        return
    params.append(dict(id=(exid << 12) | pid, name=name, lo=lo, hi=hi, fmt=fmt, off=off, shift=shift, bits=bits,
                       section=section, model=model, slot=slot, key=key or f"p{(exid << 12) | pid}"))


def region(start):
    _cur["off"] = start
    _cur["byte"] = None


# ---- Program Common (138) ----------------------------------------------------------------------
region(0)
P(1, "", 0, 0, size="16", expose=False)                       # name
P(17, "", 0, 0, size="B0-3", expose=False)                    # program category
P(18, "", 0, 0, size="^4-7", expose=False)                    # user category
P(19, "", 0, 0, size="B0-1", expose=False)                    # oscillator mode (SOLO only)
P(21, "Hold", 0, 1, "C:OFFON", "^3", "VOICE")
P(22, "Key Prio", 0, 2, "C:PRIO", "^4-5", "VOICE")
P(23, "Trigger", 0, 2, "C:TRIG", "^6-7", "VOICE")
P(24, "Trig Vel", 1, 127, "N", "B0-6", "VOICE")
P(25, "Trig Above", 0, 1, "C:ABOVE", "^7", "VOICE")
P(26, "Scale Key", 0, 11, "C:KEY12", "B0-3", "VOICE")
# 11 scales (guide CMN-4 list, PE list range 0..10; the dump table gives the 4-bit field as 0~15)
P(27, "Scale Type", 0, 10, "C:SCALE", "^4-7", "VOICE")
P(28, "Rand Pitch", 0, 99, "N", "1", "VOICE")
P(29, "", 0, 0, size="R")
EG_FIELDS = [("Start", -99, 99), ("Atk Time", 0, 99), ("Atk Level", -99, 99), ("Dcy Time", 0, 99),
             ("Break Pt", -99, 99), ("Slp Time", 0, 99), ("Sus Level", -99, 99), ("Rel Time", 0, 99),
             ("Rel Level", -99, 99), ("KT Atk", -99, 99), ("KT Dcy", -99, 99), ("KT Slope", -99, 99),
             ("KT Rel", -99, 99), ("Vel Level", -99, 99), ("Vel Atk", -99, 99), ("Vel Dcy", -99, 99),
             ("Vel Slope", -99, 99), ("Vel Rel", -99, 99)]
for e in range(4):
    for k, (n, lo, hi) in enumerate(EG_FIELDS):
        P(30 + 18 * e + k, f"EG{e + 1} {n}", lo, hi, "S" if lo < 0 else "N", "1", f"EG{e + 1}")
for l in range(4):
    b = 102 + 13 * l
    s = f"LFO{l + 1}"
    P(b + 0, f"{s} Wave", 0, 29, "C:LFOWAVE", "B0-4", s)
    P(b + 1, f"{s} KeySync", 0, 1, "C:OFFON", "^7", s)
    P(b + 2, f"{s} Mode", 0, 2, "C:LFOMODE", "^5-6", s)
    P(b + 3, f"{s} Freq", 0, 199, "N", "1", s)
    P(b + 4, f"{s} FrqKT", -99, 99, "S", "1", s)
    P(b + 5, f"{s} FrqCC1", -99, 99, "S", "1", s)
    P(b + 6, f"{s} FrqSrc", 0, 111, "C:SRC", "1", s)
    P(b + 7, f"{s} FrqInt", -99, 99, "S", "1", s)
    P(b + 8, f"{s} Offset", -99, 99, "S", "1", s)
    P(b + 9, f"{s} AmpSrc", 0, 111, "C:SRC", "1", s)
    P(b + 10, f"{s} AmpInt", -99, 99, "S", "1", s)
    P(b + 11, f"{s} Delay", 0, 99, "N", "1", s)
    P(b + 12, f"{s} FadeIn", -99, 99, "S", "1", s)
assert _cur["off"] == 138, _cur["off"]

# ---- Wave Form Generator (121) -----------------------------------------------------------------
P(154, "OSC Set", 0, 11, "C:OSCSET", "1", "OSC")
for k, (n, lo, hi) in enumerate([("Start", -99, 99), ("Atk Time", 0, 99), ("Atk Level", -99, 99), ("Dcy Time", 0, 99),
                                 ("Break Pt", -99, 99), ("Slp Time", 0, 99), (None, 0, 0), ("Rel Time", 0, 99),
                                 ("Rel Level", -99, 99)]):
    if n is None:
        P(155 + k, "", 0, 0, size="R")
    else:
        P(155 + k, f"PEG {n}", lo, hi, "S" if lo < 0 else "N", "1", "PITCH EG")
P(164, "PEG KT Lvl", -99, 99, "S", "1", "PITCH EG")
P(165, "PEG KT Time", -99, 99, "S", "1", "PITCH EG")
P(166, "PEG Vel Lvl", -99, 99, "S", "1", "PITCH EG")
P(167, "PEG Vel Tim", -99, 99, "S", "1", "PITCH EG")
P(168, "Bend +X", -60, 12, "S", "1", "BEND")
P(169, "Bend -X", -60, 12, "S", "1", "BEND")
P(170, "Bend Step+", 0, 15, "C:BSTEP", "B0-3", "BEND")
P(171, "Bend Step-", 0, 15, "C:BSTEP", "^4-7", "BEND")
P(172, "AT Bend", -12, 12, "S", "1", "BEND")
P(173, "Porta Mode", 0, 1, "C:FINGER", "B7", "BEND")
P(174, "Porta Time", 0, 99, "N", "^0-6", "BEND")
P(175, "Porta VelCtl", -99, 99, "S", "1", "BEND")
OSC_BASE = {1: 158, 2: 206}
SETTING = {1: 172, 2: 220}
for o in (1, 2):
    b = 176 + 14 * (o - 1)
    s = f"OSC{o}"
    P(b + 0, f"O{o} Octave", 0, 3, "OCT", "1", s)
    P(b + 1, f"O{o} Semi", -12, 12, "S", "1", s)
    P(b + 2, f"O{o} Fine", -50, 50, "CENT", "1", s)
    P(b + 3, f"O{o} FreqOfs", -100, 100, "HZ", "1", s)
    P(b + 4, f"O{o} SlpLoKey", 0, 127, "NOTE", "1", s)
    P(b + 5, f"O{o} SlpHiKey", 0, 127, "NOTE", "1", s)
    P(b + 6, f"O{o} LoSlope", -50, 100, "SLOPE", "1", s)
    P(b + 7, f"O{o} HiSlope", -50, 100, "SLOPE", "1", s)
    P(b + 8, f"O{o} P.LFO", 7, 10, "LFO", "1", s)
    P(b + 9, f"O{o} P.LFOInt", -99, 99, "S", "1", s)
    P(b + 10, f"O{o} P.LFO AT", -99, 99, "S", "1", s)
    P(b + 11, f"O{o} P.LFOCC1", -99, 99, "S", "1", s)
    P(b + 12, f"O{o} P.ModSrc", 0, 111, "C:SRC", "1", s)
    P(b + 13, f"O{o} P.ModInt", -99, 99, "S", "1", s)
    assert _cur["off"] == SETTING[o], _cur["off"]
    _cur["off"] += 34                                           # model settings (below)
P(204, "Sub Source", 0, 1, "C:SUBSRC", "B7", "SUB/NOISE")
P(205, "Sub Semi", -24, 24, "S", "^0-6", "SUB/NOISE")
P(206, "Sub Fine", -50, 50, "CENT", "1", "SUB/NOISE")
P(207, "Sub Wave", 0, 3, "C:SUBWAVE", "1", "SUB/NOISE")
P(208, "Noise LPF", 0, 99, "N", "1", "SUB/NOISE")
P(209, "Noise LPF KT", -99, 99, "S", "1", "SUB/NOISE")
assert _cur["off"] == 259, _cur["off"]

# ---- Wave Shape (26) ---------------------------------------------------------------------------
for o in (1, 2):
    b = 210 + 14 * (o - 1)
    s = f"SHAPE{o}"
    P(b + 0, f"WS{o} InGain", 0, 99, "N", "1", s)
    P(b + 1, f"WS{o} InGnSrc", 0, 111, "C:SRC", "1", s)
    P(b + 2, f"WS{o} InGnInt", -99, 99, "S", "1", s)
    P(b + 3, f"WS{o} InOffs", -99, 99, "S", "1", s)
    P(b + 4, "", 0, 0, size="R")
    P(b + 5, "", 0, 0, size="R")
    P(b + 6, f"WS{o} Fdback", 0, 99, "N", "1", s)
    P(b + 7, f"WS{o} XLoop", 0, 99, "N", "1", s)
    P(b + 8, f"WS{o} Table", 0, 1, "C:SHAPETBL", "B7", s)
    P(b + 9, f"WS{o} Shape", 0, 99, "N", "^0-6", s)
    P(b + 10, f"WS{o} ShpSrc", 0, 111, "C:SRC", "1", s)
    P(b + 11, f"WS{o} ShpInt", -99, 99, "S", "1", s)
    P(b + 12, f"WS{o} OutGain", 0, 99, "N", "1", s)
    P(b + 13, f"WS{o} Thru", 0, 99, "N", "1", s)
assert _cur["off"] == 285, _cur["off"]

# ---- Mixer (30) --------------------------------------------------------------------------------
for k, n in enumerate(["O1>1", "O1>2", "O2>1", "O2>2", "Sub>1", "Sub>2", "Nz>1", "Nz>2", "FB>1", "FB>2"]):
    b = 238 + 3 * k
    P(b, f"Mx {n} Lvl", 0, 99, "N", "1", "MIXER")
    P(b + 1, f"Mx {n} Src", 0, 111, "C:SRC", "1", "MIXER")
    P(b + 2, f"Mx {n} Int", -99, 99, "S", "1", "MIXER")
assert _cur["off"] == 315, _cur["off"]

# ---- Filter (33) -------------------------------------------------------------------------------
P(268, "F Routing", 0, 2, "C:ROUTING", "1", "FILTER")
for f in (1, 2):
    b = 269 + 16 * (f - 1)
    s = f"FILTER{f}"
    P(b + 0, f"F{f} Type", 0, 4, "C:FTYPE", "1", s)
    P(b + 1, f"F{f} InTrim", 0, 99, "N", "1", s)
    P(b + 2, f"F{f} Cutoff", 0, 99, "N", "1", s)
    P(b + 3, f"F{f} KT LoKey", 0, 127, "NOTE", "1", s)
    P(b + 4, f"F{f} KT HiKey", 0, 127, "NOTE", "1", s)
    P(b + 5, f"F{f} KT LoInt", -99, 99, "S", "1", s)
    P(b + 6, f"F{f} KT HiInt", -99, 99, "S", "1", s)
    P(b + 7, f"F{f} EG", 1, 6, "EG", "1", s)
    P(b + 8, f"F{f} EG Int", -99, 99, "S", "1", s)
    P(b + 9, f"F{f} LFO", 7, 10, "LFO", "1", s)
    P(b + 10, f"F{f} LFO Int", -99, 99, "S", "1", s)
    P(b + 11, f"F{f} ModSrc", 0, 111, "C:SRC", "1", s)
    P(b + 12, f"F{f} ModInt", -99, 99, "S", "1", s)
    P(b + 13, f"F{f} Reso", 0, 99, "N", "1", s)
    P(b + 14, f"F{f} ResoSrc", 0, 111, "C:SRC", "1", s)
    P(b + 15, f"F{f} ResoInt", -99, 99, "S", "1", s)
assert _cur["off"] == 348, _cur["off"]

# ---- Amplitude (36) ----------------------------------------------------------------------------
for a in (1, 2):
    b = 301 + 9 * (a - 1)
    s = f"AMP{a}"
    P(b + 0, f"A{a} Level", 0, 99, "N", "1", s)
    P(b + 1, f"A{a} KT LoKey", 0, 127, "NOTE", "1", s)
    P(b + 2, f"A{a} KT HiKey", 0, 127, "NOTE", "1", s)
    P(b + 3, f"A{a} KT LoInt", -99, 99, "S", "1", s)
    P(b + 4, f"A{a} KT HiInt", -99, 99, "S", "1", s)
    P(b + 5, f"A{a} EG", 1, 6, "EG", "1", s)
    P(b + 6, f"A{a} EG Int", -99, 99, "S", "1", s)
    P(b + 7, f"A{a} ModSrc", 0, 111, "C:SRC", "1", s)
    P(b + 8, f"A{a} ModInt", -99, 99, "S", "1", s)
AEG = [("Start", 0, 99), ("Atk Time", 0, 99), ("Atk Level", 0, 99), ("Dcy Time", 0, 99), ("Break Pt", 0, 99),
       ("Slp Time", 0, 99), ("Sus Level", 0, 99), ("Rel Time", 0, 99), (None, 0, 0), ("KT Atk", -99, 99),
       ("KT Dcy", -99, 99), ("KT Slope", -99, 99), ("KT Rel", -99, 99), ("Vel Level", -99, 99),
       ("Vel Atk", -99, 99), ("Vel Dcy", -99, 99), ("Vel Slope", -99, 99), ("Vel Rel", -99, 99)]
for k, (n, lo, hi) in enumerate(AEG):
    if n is None:
        P(319 + k, "", 0, 0, size="R")
    else:
        P(319 + k, f"AEG {n}", lo, hi, "S" if lo < 0 else "N", "1", "AMP EG")
assert _cur["off"] == 384, _cur["off"]

# ---- Effects -----------------------------------------------------------------------------------
P(337, "Dist Gain", 0, 99, "N", "1", "DIST/WAH")
P(338, "", 0, 0, size="R")
P(339, "", 0, 0, size="R")
P(340, "Dist Tone", 0, 99, "N", "1", "DIST/WAH")
P(341, "Dist Level", 0, 99, "N", "1", "DIST/WAH")
P(342, "Dist Bal", 0, 100, "PCT", "1", "DIST/WAH")
P(343, "Dist BalSrc", 0, 111, "C:SRC", "1", "DIST/WAH")
P(344, "Dist BalInt", -99, 99, "S", "1", "DIST/WAH")
P(345, "Wah Reso", 0, 99, "N", "1", "DIST/WAH")
P(346, "Wah LoFreq", 0, 99, "N", "1", "DIST/WAH")
P(347, "Wah HiFreq", 0, 99, "N", "1", "DIST/WAH")
P(348, "Wah SwpSrc", 0, 111, "C:SRC", "1", "DIST/WAH")
P(349, "Wah SwpDir", 0, 1, "C:SWEEPDIR", "1", "DIST/WAH")
P(350, "Wah Level", 0, 99, "N", "1", "DIST/WAH")
P(351, "Wah Bal", 0, 100, "PCT", "1", "DIST/WAH")
P(352, "Wah BalSrc", 0, 111, "C:SRC", "1", "DIST/WAH")
P(353, "Wah BalInt", -99, 99, "S", "1", "DIST/WAH")
P(354, "DlyRev Sel", 0, 1, "C:DLYSEL", "1", "DELAY/REV")
P(355, "Cho Time", 0, 99, "N", "1", "DELAY/REV")
P(356, "Cho Fdback", -99, 99, "S", "1", "DELAY/REV")
P(357, "Cho LFO", 7, 10, "LFO", "1", "DELAY/REV")
P(358, "Cho Depth", 0, 99, "N", "1", "DELAY/REV")
P(359, "Cho DepSrc", 0, 111, "C:SRC", "1", "DELAY/REV")
P(360, "Cho DepInt", -99, 99, "S", "1", "DELAY/REV")   # guide says 0~99; factory data is signed
P(361, "Cho Bal", 0, 100, "PCT", "1", "DELAY/REV")
P(362, "Cho BalSrc", 0, 111, "C:SRC", "1", "DELAY/REV")
P(363, "Cho BalInt", -99, 99, "S", "1", "DELAY/REV")
P(364, "Dly Time", 0, 99, "N", "1", "DELAY/REV")
P(365, "Dly Fdback", 0, 99, "N", "1", "DELAY/REV")
P(366, "Dly HiDamp", 0, 99, "N", "1", "DELAY/REV")
P(367, "Dly Bal", 0, 100, "PCT", "1", "DELAY/REV")
P(368, "Dly BalSrc", 0, 111, "C:SRC", "1", "DELAY/REV")
P(369, "Dly BalInt", -99, 99, "S", "1", "DELAY/REV")
P(370, "Rev PreDly", 0, 99, "N", "1", "DELAY/REV")
P(371, "Rev Time", 0, 99, "N", "1", "DELAY/REV")
P(372, "Rev HiDamp", 0, 99, "N", "1", "DELAY/REV")
P(373, "Rev Bal", 0, 100, "PCT", "1", "DELAY/REV")
P(374, "Rev BalSrc", 0, 111, "C:SRC", "1", "DELAY/REV")
P(375, "Rev BalInt", -99, 99, "S", "1", "DELAY/REV")
P(376, "EQ Hi Freq", 0, 49, "N", "1", "EQ/OUT")
P(377, "EQ Hi Q", 0, 29, "N", "1", "EQ/OUT")
P(378, "EQ Hi Gain", -18, 18, "DB", "1", "EQ/OUT")
P(379, "EQ Lo Freq", 0, 49, "N", "1", "EQ/OUT")
P(380, "EQ Lo Q", 0, 29, "N", "1", "EQ/OUT")
P(381, "EQ Lo Gain", -18, 18, "DB", "1", "EQ/OUT")
P(382, "Pan", 0, 127, "N", "1", "EQ/OUT")
P(383, "Pan Src", 0, 111, "C:SRC", "1", "EQ/OUT")
P(384, "Pan Int", -99, 99, "S", "1", "EQ/OUT")
P(385, "Out Level", 0, 127, "N", "1", "EQ/OUT")
P(386, "", 0, 0, size="R")
P(387, "", 0, 0, size="R")
assert _cur["off"] == 435, _cur["off"]

# ---- Oscillator models (ExID 1 = OSC1 settings at 172, 2 = OSC2 settings at 220) -------------
def model(o, m, specs):
    region(SETTING[o])
    tag = f"O{o}{['', 'S', 'C', 'V', 'M', 'B', 'R', 'P'][m]}"
    for spec in specs:
        pid, n, lo, hi, fmt, size = spec
        if n is None:
            P(pid, "", 0, 0, size=size if size != "1" else "R", exid=o)
            continue
        P(pid, f"{tag} {n}", lo, hi, fmt, size, f"O{o} {MODELS[m].upper()}", model=m, slot=o, exid=o)
    assert _cur["off"] <= SETTING[o] + 34


STD = [(388, "Wave", 0, 1, "C:STDWAVE", "1"), (389, "Edge", 0, 99, "N", "1"), (390, "WaveLvl", 0, 99, "N", "1"),
       (391, "RampLvl", 0, 99, "N", "1"), (392, "WaveForm", -99, 99, "S", "1"), (393, "WF LFO", 7, 10, "LFO", "1"),
       (394, "WF LFOInt", -99, 99, "S", "1"), (395, "WF Src", 0, 111, "C:SRC", "1"), (396, "WF Int", -99, 99, "S", "1")]
COMB = [(397, "Noise", 0, 99, "N", "1"), (398, "InWave", 0, 2, "C:COMBIN", "1"), (399, "InWvLvl", 0, 99, "N", "1"),
        (400, "InGain", 0, 99, "N", "1"), (401, "Feedback", 0, 99, "N", "1"), (402, "FB EG", 1, 6, "EG", "1"),
        (403, "FB EGInt", -99, 99, "S", "1"), (404, "FB Src", 0, 111, "C:SRC", "1"), (405, "FB Int", -99, 99, "S", "1"),
        (406, "LoopLPF", 0, 99, "N", "1")]
VPM = [(407, "CarWave", 0, 3, "C:VPMCAR", "1"), (408, "CarLvl", 0, 99, "N", "1"), (409, "CarEG", 1, 6, "EG", "1"),
       (410, "CarEGInt", -99, 99, "S", "1"), (411, "CarSrc", 0, 111, "C:SRC", "1"), (412, "CarInt", -99, 99, "S", "1"),
       (413, "TblShape", 0, 99, "N", "1"), (414, "TblLFO", 7, 10, "LFO", "1"), (415, "TblLFOInt", -99, 99, "S", "1"),
       (416, "TblSrc", 0, 111, "C:SRC", "1"), (417, "TblInt", -99, 99, "S", "1"), (418, "FB Gain", 0, 99, "N", "1"),
       (419, "ModSemi", -12, 96, "S", "1"), (420, "ModFine", -50, 50, "CENT", "1"), (421, "ModPchKT", -99, 99, "S", "1"),
       (422, "ModPSrc", 0, 111, "C:SRC", "1"), (423, "ModPInt", -99, 99, "S", "1"), (424, "ModWave", 0, 4, "C:VPMMOD", "1"),
       (425, "ModLvl", 0, 99, "N", "1"), (426, "ModEG", 1, 6, "EG", "1"), (427, "ModEGInt", -99, 99, "S", "1"),
       (428, "ModEGKT", -99, 99, "S", "1"), (429, "ModLSrc", 0, 111, "C:SRC", "1"), (430, "ModLInt", -99, 99, "S", "1")]
MOD = [(431, "Type", 0, 2, "C:MODTYPE", "1"), (432, "Input", 0, 2, "C:MODIN", "1"), (433, "RingCar", 0, 2, "C:RINGCAR", "1"),
       (434, "XCar", 0, 2, "C:RINGCAR", "1"), (435, "XDepth", 0, 99, "N", "1"), (436, "XDepEG", 1, 6, "EG", "1"),
       (437, "XDepEGInt", -99, 99, "S", "1"), (438, "XDepSrc", 0, 111, "C:SRC", "1"), (439, "XDepInt", -99, 99, "S", "1"),
       (440, "SyncWave", 0, 1, "C:SYNCWAVE", "1"), (441, "SyncShape", 0, 99, "N", "1")]
BRASS = [(442, "Instr", 0, 3, "C:BRASSTYPE", "1"), (443, "BendCtl", 0, 111, "C:SRC", "1"), (444, "BendInt", 0, 12, "N", "B0-5"),
         (445, "BendDir", 0, 2, "C:SBDIR", "^6-7"), (446, "PrsEG", 1, 6, "EG", "1"), (447, "PrsEGInt", -99, 99, "S", "1"),
         (448, "PrsEGSrc", 0, 111, "C:SRC", "1"), (449, "PrsEGMod", -99, 99, "S", "1"), (450, "PrsLFO", 7, 10, "LFO", "1"),
         (451, "PrsLFOInt", -99, 99, "S", "1"), (452, "PrsSrc", 0, 111, "C:SRC", "1"), (453, "PrsInt", -99, 99, "S", "1"),
         (454, None, 0, 0, "", "1"), (455, None, 0, 0, "", "1"), (456, "Lip", 0, 99, "N", "1"),
         (457, "LipSrc", 0, 111, "C:SRC", "1"), (458, "LipInt", -99, 99, "S", "1"), (459, None, 0, 0, "", "1"),
         (460, None, 0, 0, "", "1"), (461, "Bell", 0, 1, "C:BELL", "1"), (462, "BellTone", 0, 99, "N", "1"),
         (463, "BellReso", 0, 99, "N", "1"), (464, "Noise", 0, 99, "N", "1")]
REED = [(477, "Instr", 0, 12, "C:REEDTYPE", "1"), (478, "BendCtl", 0, 111, "C:SRC", "1"), (479, "BendInt", 0, 12, "N", "B0-5"),
        (480, "BendDir", 0, 2, "C:SBDIR", "^6-7"), (481, "PrsEG", 1, 6, "EG", "1"), (482, "PrsEGInt", 0, 99, "N", "1"),
        (483, "PrsEGSrc", 0, 111, "C:SRC", "1"), (484, "PrsEGMod", -99, 99, "S", "1"), (485, "PrsLFO", 7, 10, "LFO", "1"),
        (486, "PrsLFOInt", -99, 99, "S", "1"), (487, "PrsSrc", 0, 111, "C:SRC", "1"), (488, "PrsInt", -99, 99, "S", "1"),
        (489, None, 0, 0, "", "1"), (490, None, 0, 0, "", "1"), (491, "ReedSrc", 0, 111, "C:SRC", "1"),
        (492, "ReedInt", -99, 99, "S", "1"), (493, None, 0, 0, "", "1"), (494, None, 0, 0, "", "1"),
        (495, None, 0, 0, "", "1"), (496, "Noise", 0, 99, "N", "1"), (497, None, 0, 0, "", "1"), (498, None, 0, 0, "", "1")]
# Reed Noise Level is parameter 496 (record byte 190), not 498 as the dump table says: the V55 sends
# 496 for it (Performance Editor #250) with range 0..99, and byte 192 holds signed values in the
# factory Reed programs (checked on the oracle).
PLUCK = [(512, "AtkLvl", 0, 99, "N", "1"), (513, "AtkLvlVel", -99, 99, "S", "1"), (514, "AtkNoise", 0, 99, "N", "1"),
         (515, "AtkNzVel", -99, 99, "S", "1"), (516, "NzFilter", 0, 2, "C:PNFILT", "1"), (517, "NzCutoff", 0, 99, "N", "1"),
         (518, "NzCutVel", -99, 99, "S", "1"), (519, "NzReso", 0, 99, "N", "1"), (520, "CurveUp", 0, 99, "N", "1"),
         (521, "CrvUpVel", -99, 99, "S", "1"), (522, "CurveDn", 0, 99, "N", "1"), (523, "CrvDnVel", -99, 99, "S", "1"),
         (524, "AtkEdge", 0, 99, "N", "1"), (525, "StrPos", 0, 99, "N", "1"), (526, "StrPosVel", -99, 99, "S", "1"),
         (527, "StrPosSrc", 0, 111, "C:SRC", "1"), (528, "StrPosInt", -99, 99, "S", "1"), (529, "StrLoss", 0, 99, "N", "1"),
         (530, "StrLossKT", -99, 99, "S", "1"), (531, "LossSrc", 0, 111, "C:SRC", "1"), (532, "LossInt", -99, 99, "S", "1"),
         (533, "Inharm", 0, 99, "N", "1"), (534, "InharmKT", -99, 99, "S", "1"), (535, "Decay", 0, 99, "N", "1"),
         (536, "DecayKT", -99, 99, "S", "1"), (537, "Release", 0, 99, "N", "1"), (538, "ReleaseKT", -99, 99, "S", "1")]
for o in (1, 2):
    model(o, 1, STD)
    model(o, 2, COMB)
    model(o, 3, VPM)
    model(o, 4, MOD)
model(1, 5, BRASS)
model(1, 6, REED)
model(1, 7, PLUCK)

# ---- emit --------------------------------------------------------------------------------------
names = [p["name"] for p in params]
dups = {n for n in names if names.count(n) > 1}
assert not dups, dups
too_long = [n for n in names if len(n) > 13]
assert not too_long, too_long
lists_used = sorted({p["fmt"][2:] for p in params if p["fmt"].startswith("C:")})
lines = ["// Generated by tools/gen_params.py from the Korg Prophecy Parameter Guide (MIDI Implementation).",
         "// Public documentation, no firmware data. Do not edit by hand.", ""]
for ln in lists_used:
    items = ", ".join('"' + s.replace('"', '\\"') + '"' for s in LISTS[ln])
    lines.append(f"static const char *const kList_{ln}[] = {{ {items} }};")
lines.append("")
lines.append("const Def kDefs[kCount] = {")
FMT = {"N": "Num", "S": "Signed", "SRC": "Choice", "LFO": "Choice", "EG": "Choice", "NOTE": "Note", "OCT": "Oct",
       "HZ": "Hz", "SLOPE": "Slope", "PCT": "Pct", "DB": "Db", "CENT": "Cent"}
for p in params:
    f = p["fmt"]
    lst, base = "nullptr", 0
    if f.startswith("C:"):
        fm, lst = "Choice", f"kList_{f[2:]}"
        base = p["lo"]
    elif f in ("LFO", "EG"):
        fm, lst, base = "Choice", f"kList_{f}", p["lo"]
        if f"{f}" not in lists_used:
            lists_used.append(f)
    else:
        fm = FMT[f]
    n = len(LISTS[lst[6:]]) if lst != "nullptr" else 0
    lines.append(f'\t{{ 0x{p["id"]:04x}, "{p["key"]}", "{p["name"]}", "{p["section"]}", {p["lo"]}, {p["hi"]}, '
                 f'{p["off"]}, {p["shift"]}, {p["bits"]}, {p["model"]}, {p["slot"]}, Fmt::{fm}, {lst}, {n}, {base} }},')
lines.append("};")
lines.append(f"static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == kCount, \"update kCount in params.h to {len(params)}\");")
# LFO/EG lists may have been added after the list emission: emit them up front
extra = [l for l in ("LFO", "EG") if l in lists_used and f"static const char *const kList_{l}[]" not in "\n".join(lines)]
for l in extra:
    items = ", ".join('"' + s + '"' for s in LISTS[l])
    lines.insert(3, f"static const char *const kList_{l}[] = {{ {items} }};")
# Performance Editor assign list (PE number -> program parameter id), from tools/pe_assign.py
sys.path.insert(0, str(REPO / "tools"))
from pe_assign import ASSIGN  # noqa: E402
missing_pe = [k for k, v in enumerate(ASSIGN) if v and v not in {p["id"] for p in params}]
assert not missing_pe, missing_pe
lines.append("")
lines.append("// Performance Editor assign list (Parameter Guide 6.4): PE number -> parameter id (0 = none).")
lines.append("const std::uint16_t kPeAssign[256] = {")
for k in range(0, 256, 8):
    lines.append("\t" + " ".join(f"0x{v:04x}," for v in ASSIGN[k:k + 8]))
lines.append("};")
OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(f"wrote {OUT}: {len(params)} parameters")
