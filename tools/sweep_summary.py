#!/usr/bin/env python3
"""Summarise a program sweep (reference/wsl/sweep_programs.sh).

Usage: sweep_summary.py <sweep root with p000..p127> <dest dir under data/traces/>

For each run: the program image active on each DSP when the note starts (12 s), and the audio
peak while it sounds. Writes <dest>/summary.json and <dest>/programs/<dsp>_<sha12>.hex
(unique images). Destination must stay under data/ (gitignored, firmware-derived).
"""
from __future__ import annotations

import array
import json
import math
import sys
import wave
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from trace_summary import parse_trace, program_image  # noqa: E402

NOTE_ON = 12.0
NOTE_WINDOW = (12.0, 15.0)


def peak_dbfs(wav_path: Path) -> float | None:
    if not wav_path.exists():
        return None
    with wave.open(str(wav_path), "rb") as w:
        rate, ch = w.getframerate(), w.getnchannels()
        w.setpos(int(NOTE_WINDOW[0] * rate))
        pcm = array.array("h", w.readframes(int((NOTE_WINDOW[1] - NOTE_WINDOW[0]) * rate)))
    peak = max((abs(s) for s in pcm), default=0)
    return round(20 * math.log10(peak / 32768), 1) if peak else None


def main() -> int:
    root, dest = Path(sys.argv[1]), Path(sys.argv[2])
    progs_dir = dest / "programs"
    progs_dir.mkdir(parents=True, exist_ok=True)
    summary = {}
    users = defaultdict(list)       # (dsp, sha) -> [program numbers]
    sizes = {}
    for run in sorted(root.glob("p[0-9][0-9][0-9]")):
        num = int(run.name[1:])
        entry = {"peak_dbfs": peak_dbfs(run / "out.wav")}
        for dsp in ("dsp1", "dsp2", "dsp3"):
            trace = run / f"host.{dsp}.csv"
            if not trace.exists():
                entry[dsp] = None
                continue
            loads, _, _ = parse_trace(trace)
            active = [ld for ld in loads if ld["t"] <= NOTE_ON]
            if not active:
                entry[dsp] = None
                continue
            image, sha = program_image(active[-1])
            entry[dsp] = sha[:12]
            users[(dsp, sha[:12])].append(num)
            sizes[(dsp, sha[:12])] = len(image)
            out = progs_dir / f"{dsp}_{sha[:12]}.hex"
            if not out.exists():
                out.write_text("".join(f"{w:06X}\n" for w in image))
        summary[num] = entry
    (dest / "summary.json").write_text(json.dumps(summary, indent=1))

    silent = [n for n, e in summary.items() if e["peak_dbfs"] is None]
    print(f"runs: {len(summary)}, silent during note: {len(silent)} {silent[:20]}")
    for dsp in ("dsp1", "dsp2", "dsp3"):
        keys = sorted((k for k in users if k[0] == dsp), key=lambda k: -len(users[k]))
        print(f"== {dsp}: {len(keys)} distinct active program(s)")
        for k in keys:
            nums = users[k]
            print(f"   {k[1]}  words={sizes[k]:3}  used by {len(nums):3} programs, e.g. {nums[:10]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
