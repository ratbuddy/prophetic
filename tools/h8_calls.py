#!/usr/bin/env python3
"""Decode H8 register snapshots (KPDEV_H8_CALLTRACE) into param_dispatch triples.

Usage: h8_calls.py <calls.bin> <out.tsv>

Records: {u32 pc, u32 cycle, u32 er0..er7}. Pairs each param_dispatch entry (0x018252: r1 = slot,
er0 = input) with the following result store (0x0182b0: er0 = compute result) and dsp_cload_write
(0x019e0a: er0 = value, r1l = dsp, r2l = coefficient address). Writes one TSV row per call:
  cycle slot input result dsp sa sent
Rows are firmware-derived behaviour samples: keep the TSV under data/.
"""
from __future__ import annotations

import struct
import sys
from collections import Counter
from pathlib import Path

REC = struct.Struct("<10I")
DISPATCH, CALL, RESULT, WRITE = 0x018252, 0x0182A2, 0x0182B0, 0x019E0A


def main() -> int:
    recs = list(REC.iter_unpack(Path(sys.argv[1]).read_bytes()))
    rows, pending, bad = [], None, Counter()
    for pc, cyc, *er in recs:
        if pc == DISPATCH:
            if pending:
                bad["dispatch without write"] += 1
            pending = {"cycle": cyc, "slot": er[1] & 0xFFFF, "input": er[0], "result": None,
                       "fn": 0, "aux4": 0, "aux3": 0, "call_in": 0}
        elif pc == CALL and pending:
            pending.update(fn=er[2] & 0xFFFFFF, aux4=er[4], aux3=er[3], call_in=er[0])
        elif pc == RESULT and pending:
            pending["result"] = er[0]
        elif pc == WRITE:
            if pending and pending["result"] is not None:
                pending.update(dsp=er[1] & 0xFF, sa=er[2] & 0xFF, sent=er[0])
                rows.append(pending)
            else:
                bad["write outside dispatch"] += 1
            pending = None
    with open(sys.argv[2], "w") as f:
        f.write("cycle\tslot\tinput\tfn\tcall_in\taux4\taux3\tresult\tdsp\tsa\tsent\n")
        for r in rows:
            f.write(f"{r['cycle']}\t{r['slot']}\t{r['input']:08x}\t{r['fn']:06x}\t{r['call_in']:08x}\t{r['aux4']:08x}\t"
                    f"{r['aux3']:08x}\t{r['result']:08x}\t{r['dsp']}\t{r['sa']:02x}\t{r['sent']:08x}\n")
    slots = Counter(r["slot"] for r in rows)
    print(f"records={len(recs)} dispatch calls={len(rows)} distinct slots={len(slots)} anomalies={dict(bad)}")
    print("result==sent for all:", all(r["result"] == r["sent"] for r in rows))
    print("busiest slots:", ", ".join(f"{s}x{c}" for s, c in slots.most_common(12)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
