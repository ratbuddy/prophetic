#!/usr/bin/env python3
"""Per-window peak/RMS (dBFS) of a 16-bit PCM WAV. Usage: py tools/wav_stats.py <file.wav> [window_s=0.5]"""
from __future__ import annotations

import array
import math
import sys
import wave


def main() -> int:
    path = sys.argv[1]
    win_s = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if width != 2:
            print(f"unsupported sample width {width}", file=sys.stderr)
            return 1
        pcm = array.array("h", w.readframes(n))
    print(f"{path}: {ch} ch, {rate} Hz, {n} frames ({n / rate:.2f} s)")
    step = int(win_s * rate) * ch
    loud = 0
    for i in range(0, len(pcm), step):
        blk = pcm[i:i + step]
        if not blk:
            break
        peak = max(abs(s) for s in blk)
        rms = math.sqrt(sum(s * s for s in blk) / len(blk))
        db = lambda v: 20 * math.log10(v / 32768) if v > 0 else -math.inf
        if peak > 0:
            loud += 1
        print(f"  {i / ch / rate:7.2f}s  peak {db(peak):7.1f} dBFS  rms {db(rms):7.1f} dBFS")
    print(f"non-silent windows: {loud}")
    return 0 if loud else 2


if __name__ == "__main__":
    sys.exit(main())
