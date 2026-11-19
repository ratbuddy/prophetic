#!/usr/bin/env python3
"""H8/300H decoder and recursive-descent traversal for the Prophecy's H8 ROM (ic22): writes the
structured IR that tools/h8xlat.py translates. Ours (written from the H8/300H instruction set), no
emulator code: the end-user build needs only the ROM and Python.

Usage: py tools/h8dec.py <ic22.bin> [--entries seeds.txt]... [--entry ADDR]... [--rom-size N] [-o ic22.ir]
(-o: write there instead of stdout)

The dump is word-swapped relative to the CPU (as dumped); this tool unswaps it. Flow: from the
64 vectors (32-bit advanced-mode table), every --entry (marked as a function) and every address in
an --entries file (one hex address per line), decode linearly and follow the immediate targets of
calls and branches; a run stops at rts / rte / jmp / sleep (the unconditional branch, named `bt`,
continues into the next bytes, as reference/h8dasm does).

IR lines (sorted by address): pc \\t length \\t flow flags \\t mnemonic [\\t op1 [\\t op2]], then
FUNC \\t addr for every call target and vector. Operands: R8:n / R16:n (the register field as
encoded: 0-7 high halves / r0-r7, 8-15 low halves / e0-e7), R32:n, IND:n (@ERn), PRE:n (@-ERn),
POST:n (@ERn+), DISP:n:d (@(d, ERn), d as 32 bits), ABS:a (20-bit address), MIND:a (@@aa:8),
TGT:a (branch / call target), IMM:v, BIT:n (bit number), CCR. Flow flags: 10000 call, 20000
return, 40000 conditional 8-bit branch (reference/h8dasm's values). Output equals
`reference/h8dasm --ir` on the same ROM and seeds, line for line (the check: py tools/h8dec.py
... | diff - data/analysis/<ir from h8dasm>).
"""
from __future__ import annotations

import sys

COND = ["bt", "bf", "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble"]
STEP_OVER, STEP_OUT, STEP_COND = 0x10000, 0x20000, 0x40000
M20 = 0xFFFFF   # 1 MiB mode: addresses wrap at A20


class Mem:
    def __init__(self, data: bytes):
        self.d = data

    def b(self, a: int) -> int:
        return self.d[a] if 0 <= a < len(self.d) else 0

    def w(self, a: int) -> int:
        return (self.b(a) << 8) | self.b(a + 1)

    def l(self, a: int) -> int:
        return (self.w(a) << 16) | self.w(a + 2)


def hx(v: int) -> str:
    return f"{v:x}"


def s8(v: int) -> int:
    return v - 0x100 if v & 0x80 else v


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def disp32(v: int) -> str:   # a displacement as the 32-bit two's complement value
    return hx(v & 0xFFFFFFFF)


def r8(n): return f"R8:{n & 15}"
def r16(n): return f"R16:{n & 15}"
def r32(n): return f"R32:{n & 7}"
def ind(n): return f"IND:{n & 7}"
def pre(n): return f"PRE:{n & 7}"
def post(n): return f"POST:{n & 7}"
def imm(v): return f"IMM:{hx(v)}"
def bit(v): return f"BIT:{hx(v & 7)}"
def tgt(v): return f"TGT:{hx(v & 0xFFFFFF)}"
def abs8(a): return f"ABS:{hx((0xFFFF00 | a) & M20)}"
def abs16(a): return f"ABS:{hx((s16(a) & 0xFFFFFFFF) & M20)}"
def abs24(a): return f"ABS:{hx(a & M20)}"


ILLEGAL = (2, "illegal", [], 0)


def mov_size(op2_hi_nibble_or_kind: str):
    return op2_hi_nibble_or_kind


def decode(m: Mem, pc: int):
    """(length, mnemonic, operands, flags) of the instruction at pc."""
    w0 = m.w(pc)
    hi, lo = w0 >> 8, w0 & 0xFF
    lh, ll = lo >> 4, lo & 15

    # ---- 0x8x-0xFx: 8-bit immediate forms
    if hi >= 0x80:
        names = {0x8: "add.b", 0x9: "addx.b", 0xA: "cmp.b", 0xB: "subx.b", 0xC: "or.b", 0xD: "xor.b", 0xE: "and.b", 0xF: "mov.b"}
        return 2, names[hi >> 4], [imm(lo), r8(hi)], 0
    # ---- 0x2x / 0x3x: mov.b @aa:8
    if hi >> 4 == 2:
        return 2, "mov.b", [abs8(lo), r8(hi)], 0
    if hi >> 4 == 3:
        return 2, "mov.b", [r8(hi), abs8(lo)], 0
    # ---- 0x4x: Bcc d:8
    if hi >> 4 == 4:
        c = hi & 15
        return 2, COND[c], [tgt(pc + 2 + s8(lo))], (STEP_COND if c >= 2 else 0)

    if hi == 0x00:
        return (2, "nop", [], 0) if lo == 0 else ILLEGAL
    if hi == 0x01:
        return decode_01(m, pc, lo)
    if hi == 0x02:
        return (2, "stc", ["CCR", r8(ll)], 0) if lh == 0 else ILLEGAL
    if hi == 0x03:
        return (2, "ldc", [r8(ll), "CCR"], 0) if lh == 0 else ILLEGAL
    if hi in (0x04, 0x05, 0x06, 0x07):
        return 2, {4: "orc", 5: "xorc", 6: "andc", 7: "ldc"}[hi], [imm(lo), "CCR"], 0
    if hi in (0x08, 0x0C, 0x0E, 0x14, 0x15, 0x16, 0x18, 0x1C, 0x1E):
        n = {0x08: "add.b", 0x0C: "mov.b", 0x0E: "addx.b", 0x14: "or.b", 0x15: "xor.b", 0x16: "and.b", 0x18: "sub.b",
             0x1C: "cmp.b", 0x1E: "subx.b"}[hi]
        return 2, n, [r8(lh), r8(ll)], 0
    if hi in (0x09, 0x0D, 0x19, 0x1D, 0x64, 0x65, 0x66):
        n = {0x09: "add.w", 0x0D: "mov.w", 0x19: "sub.w", 0x1D: "cmp.w", 0x64: "or.w", 0x65: "xor.w", 0x66: "and.w"}[hi]
        return 2, n, [r16(lh), r16(ll)], 0
    if hi in (0x0A, 0x1A):
        if lh == 0:
            return 2, "inc.b" if hi == 0x0A else "dec.b", [imm(1), r8(ll)], 0
        if lh & 8 and not ll & 8:
            return 2, "add.l" if hi == 0x0A else "sub.l", [r32(lh), r32(ll)], 0
        return ILLEGAL
    if hi in (0x0B, 0x1B):
        inc = hi == 0x0B
        if ll & 8 and lh in (0, 8, 9):
            return ILLEGAL
        table = {0x0: ("adds.l" if inc else "subs.l", 1, r32), 0x8: ("adds.l" if inc else "subs.l", 2, r32),
                 0x9: ("adds.l" if inc else "subs.l", 4, r32), 0x5: ("inc.w" if inc else "dec.w", 1, r16),
                 0xD: ("inc.w" if inc else "dec.w", 2, r16), 0x7: ("inc.l" if inc else "dec.l", 1, r32),
                 0xF: ("inc.l" if inc else "dec.l", 2, r32)}
        if lh not in table:
            return ILLEGAL
        n, v, rf = table[lh]
        if rf is r32 and ll & 8:
            return ILLEGAL
        return 2, n, [imm(v), rf(ll)], 0
    if hi in (0x0F, 0x1F):
        if lh == 0:
            return 2, "daa.b" if hi == 0x0F else "das.b", [r8(ll)], 0
        if lh & 8 and not ll & 8:
            return 2, "mov.l" if hi == 0x0F else "cmp.l", [r32(lh), r32(ll)], 0
        return ILLEGAL
    if hi in (0x10, 0x11, 0x12, 0x13):
        names = {0x10: ("shll", "shal"), 0x11: ("shlr", "shar"), 0x12: ("rotxl", "rotl"), 0x13: ("rotxr", "rotr")}[hi]
        size = {0x0: (".b", r8), 0x1: (".w", r16), 0x3: (".l", r32)}
        if lh & 7 not in size:
            return ILLEGAL
        sfx, rf = size[lh & 7]
        if rf is r32 and ll & 8:
            return ILLEGAL
        return 2, names[lh >> 3] + sfx, [rf(ll)], 0
    if hi == 0x17:
        t = {0x0: ("not.b", r8), 0x1: ("not.w", r16), 0x3: ("not.l", r32), 0x5: ("extu.w", r16), 0x7: ("extu.l", r32),
             0x8: ("neg.b", r8), 0x9: ("neg.w", r16), 0xB: ("neg.l", r32), 0xD: ("exts.w", r16), 0xF: ("exts.l", r32)}
        if lh not in t:
            return ILLEGAL
        n, rf = t[lh]
        if rf is r32 and ll & 8:
            return ILLEGAL
        return 2, n, [rf(ll)], 0
    if hi in (0x50, 0x51):
        return 2, "mulxu.b" if hi == 0x50 else "divxu.b", [r8(lh), r16(ll)], 0
    if hi in (0x52, 0x53):
        if ll & 8:
            return ILLEGAL
        return 2, "mulxu.w" if hi == 0x52 else "divxu.w", [r16(lh), r32(ll)], 0
    if hi == 0x54:
        return (2, "rts", [], STEP_OUT) if lo == 0x70 else ILLEGAL
    if hi == 0x55:
        return 2, "bsr", [tgt(pc + 2 + s8(lo))], STEP_OVER
    if hi == 0x56:
        return (2, "rte", [], STEP_OUT) if lo == 0x70 else ILLEGAL
    if hi == 0x57:
        return (2, "trapa", [imm((lo >> 4) & 3)], 0) if (lo & 0xCF) == 0 else ILLEGAL
    if hi == 0x58:
        if ll != 0:
            return ILLEGAL
        return 4, COND[lh], [tgt(pc + 4 + s16(m.w(pc + 2)))], 0
    if hi in (0x59, 0x5D):
        if ll != 0 or lh & 8:
            return ILLEGAL
        return 2, "jmp" if hi == 0x59 else "jsr", [r32(lh)], (STEP_OVER if hi == 0x5D else 0)
    if hi in (0x5A, 0x5E):
        return 4, "jmp" if hi == 0x5A else "jsr", [tgt(m.l(pc) & 0xFFFFFF)], (STEP_OVER if hi == 0x5E else 0)
    if hi in (0x5B, 0x5F):
        return 2, "jmp" if hi == 0x5B else "jsr", [f"MIND:{hx(lo)}"], (STEP_OVER if hi == 0x5F else 0)
    if hi == 0x5C:
        if lo != 0:
            return ILLEGAL
        return 4, "bsr", [tgt(pc + 4 + s16(m.w(pc + 2)))], STEP_OVER
    if hi in (0x60, 0x61, 0x62, 0x63):
        return 2, {0x60: "bset", 0x61: "bnot", 0x62: "bclr", 0x63: "btst"}[hi], [r8(lh), r8(ll)], 0
    if hi == 0x67:
        return 2, "bist" if lh & 8 else "bst", [bit(lh), r8(ll)], 0
    if hi in (0x70, 0x71, 0x72, 0x73):
        if lh & 8:
            return ILLEGAL
        return 2, {0x70: "bset", 0x71: "bnot", 0x72: "bclr", 0x73: "btst"}[hi], [bit(lh), r8(ll)], 0
    if hi in (0x74, 0x75, 0x76, 0x77):
        n = {0x74: ("bor", "bior"), 0x75: ("bxor", "bixor"), 0x76: ("band", "biand"), 0x77: ("bld", "bild")}[hi][lh >> 3]
        return 2, n, [bit(lh), r8(ll)], 0
    if hi in (0x68, 0x69):
        sz, rf = ("mov.b", r8) if hi == 0x68 else ("mov.w", r16)
        return (2, sz, [rf(ll), ind(lh)], 0) if lh & 8 else (2, sz, [ind(lh), rf(ll)], 0)
    if hi in (0x6C, 0x6D):
        sz, rf = ("mov.b", r8) if hi == 0x6C else ("mov.w", r16)
        return (2, sz, [rf(ll), pre(lh)], 0) if lh & 8 else (2, sz, [post(lh), rf(ll)], 0)
    if hi in (0x6E, 0x6F):
        sz, rf = ("mov.b", r8) if hi == 0x6E else ("mov.w", r16)
        d = f"DISP:{lh & 7}:{disp32(s16(m.w(pc + 2)))}"
        return (4, sz, [rf(ll), d], 0) if lh & 8 else (4, sz, [d, rf(ll)], 0)
    if hi in (0x6A, 0x6B):
        sz, rf = ("mov.b", r8) if hi == 0x6A else ("mov.w", r16)
        if lh == 0x0:
            return 4, sz, [abs16(m.w(pc + 2)), rf(ll)], 0
        if lh == 0x2:
            return 6, sz, [abs24(m.l(pc + 2)), rf(ll)], 0
        if lh == 0x8:
            return 4, sz, [rf(ll), abs16(m.w(pc + 2))], 0
        if lh == 0xA:
            return 6, sz, [rf(ll), abs24(m.l(pc + 2))], 0
        if hi == 0x6A and lh == 0x4:
            return 4, "movfpe", [abs16(m.w(pc + 2)), r8(ll)], 0
        if hi == 0x6A and lh == 0xC:
            return 4, "movtpe", [r8(ll), abs16(m.w(pc + 2))], 0
        return ILLEGAL
    if hi == 0x78:
        return decode_78(m, pc, lo, 0, "")
    if hi == 0x79 or hi == 0x7A:
        n = ["mov", "add", "cmp", "sub", "or", "xor", "and"]
        if lh > 6:
            return ILLEGAL
        if hi == 0x79:
            return 4, n[lh] + ".w", [imm(m.w(pc + 2)), r16(ll)], 0
        if ll & 8:
            return ILLEGAL
        return 6, n[lh] + ".l", [imm(m.l(pc + 2)), r32(ll)], 0
    if hi == 0x7B:
        w1 = m.w(pc + 2)
        if lo == 0x5C and w1 == 0x598F:
            return 4, "eepmov.b", [], 0
        if lo == 0xD4 and w1 == 0x598F:
            return 4, "eepmov.w", [], 0
        return ILLEGAL
    if hi in (0x7C, 0x7D, 0x7E, 0x7F):
        return decode_bitmem(m, pc, hi, lo)
    return ILLEGAL


def decode_bitmem(m: Mem, pc: int, hi: int, lo: int):
    """7C / 7D (@ERd) and 7E / 7F (@aa:8) bit manipulation: the second word holds the operation."""
    w1 = m.w(pc + 2)
    op, b1 = w1 >> 8, w1 & 0xFF
    if hi in (0x7C, 0x7D):
        if lo & 0x8F:
            return ILLEGAL
        ea = ind(lo >> 4)
    else:
        ea = abs8(lo)
    if b1 & 0x0F:
        return ILLEGAL
    n = b1 >> 4
    test = hi in (0x7C, 0x7E)
    if test:
        if op == 0x63:
            return 4, "btst", [r8(n), ea], 0
        if op == 0x73:
            return (4, "btst", [bit(n), ea], 0) if not n & 8 else ILLEGAL
        if op in (0x74, 0x75, 0x76, 0x77):
            nm = {0x74: ("bor", "bior"), 0x75: ("bxor", "bixor"), 0x76: ("band", "biand"), 0x77: ("bld", "bild")}[op][n >> 3]
            return 4, nm, [bit(n), ea], 0
        return ILLEGAL
    if op in (0x60, 0x61, 0x62):
        return 4, {0x60: "bset", 0x61: "bnot", 0x62: "bclr"}[op], [r8(n), ea], 0
    if op in (0x70, 0x71, 0x72):
        return (4, {0x70: "bset", 0x71: "bnot", 0x72: "bclr"}[op], [bit(n), ea], 0) if not n & 8 else ILLEGAL
    if op == 0x67:
        return 4, "bist" if n & 8 else "bst", [bit(n), ea], 0
    return ILLEGAL


def decode_78(m: Mem, pc: int, lo: int, base: int, pfx: str, size: str = ""):
    """78 s0 6A/6B ...: @(d:24, ERs) moves (base = 2 with the 01 00 prefix of mov.l)."""
    p = pc + base
    w1 = m.w(p + 2)
    op, b1 = w1 >> 8, w1 & 0xFF
    if base == 0 and op not in (0x6A, 0x6B):
        return ILLEGAL
    if base == 2 and op != 0x6B:
        return ILLEGAL
    k = b1 >> 4
    reg = b1 & 15
    if k not in (0x2, 0xA):
        return ILLEGAL
    # the register byte: 0rrr 0000, except mov.l ERs, @(d:24, ERd), whose ERd field reads 1ddd 0000
    if lo & 0x0F or bool(lo & 0x80) != (base == 2 and k == 0xA):
        return ILLEGAL
    s = lo >> 4
    d = f"DISP:{s & 7}:{hx(m.l(p + 4))}"
    if base == 2:
        if reg & 8:
            return ILLEGAL
        sz, rf = "mov.l", r32
    else:
        sz, rf = ("mov.b", r8) if op == 0x6A else ("mov.w", r16)
    ln = base + 8
    return (ln, sz, [d, rf(reg)], 0) if k == 0x2 else (ln, sz, [rf(reg), d], 0)


def decode_01(m: Mem, pc: int, lo: int):
    w1 = m.w(pc + 2)
    op, b1 = w1 >> 8, w1 & 0xFF
    bh, bl = b1 >> 4, b1 & 15
    if lo == 0x80:
        return 2, "sleep", [], 0
    if lo == 0x00:   # mov.l with memory operands
        if op == 0x69:
            if bl & 8:
                return ILLEGAL
            return (4, "mov.l", [r32(bl), ind(bh)], 0) if bh & 8 else (4, "mov.l", [ind(bh), r32(bl)], 0)
        if op == 0x6D:
            if bl & 8:
                return ILLEGAL
            return (4, "mov.l", [r32(bl), pre(bh)], 0) if bh & 8 else (4, "mov.l", [post(bh), r32(bl)], 0)
        if op == 0x6F:
            if bl & 8:
                return ILLEGAL
            d = f"DISP:{bh & 7}:{disp32(s16(m.w(pc + 4)))}"
            return (6, "mov.l", [r32(bl), d], 0) if bh & 8 else (6, "mov.l", [d, r32(bl)], 0)
        if op == 0x6B:
            if bl & 8:
                return ILLEGAL
            if bh == 0x0:
                return 6, "mov.l", [abs16(m.w(pc + 4)), r32(bl)], 0
            if bh == 0x2:
                return 8, "mov.l", [abs24(m.l(pc + 4)), r32(bl)], 0
            if bh == 0x8:
                return 6, "mov.l", [r32(bl), abs16(m.w(pc + 4))], 0
            if bh == 0xA:
                return 8, "mov.l", [r32(bl), abs24(m.l(pc + 4))], 0
            return ILLEGAL
        if op == 0x78:
            return decode_78(m, pc, b1, 2, "")
        return ILLEGAL
    if lo == 0x40:   # ldc.w / stc.w with memory operands
        if op == 0x69 and bl == 0:
            return (4, "stc.w", ["CCR", ind(bh)], 0) if bh & 8 else (4, "ldc.w", [ind(bh), "CCR"], 0)
        if op == 0x6D and bl == 0:
            return (4, "stc.w", ["CCR", pre(bh)], 0) if bh & 8 else (4, "ldc.w", [post(bh), "CCR"], 0)
        if op == 0x6F and bl == 0:
            d = f"DISP:{bh & 7}:{disp32(s16(m.w(pc + 4)))}"
            return (6, "stc.w", ["CCR", d], 0) if bh & 8 else (6, "ldc.w", [d, "CCR"], 0)
        if op == 0x6B and bl == 0:
            if bh == 0x0:
                return 6, "ldc.w", [abs16(m.w(pc + 4)), "CCR"], 0
            if bh == 0x2:
                return 8, "ldc.w", [abs24(m.l(pc + 4)), "CCR"], 0
            if bh == 0x8:
                return 6, "stc.w", ["CCR", abs16(m.w(pc + 4))], 0
            if bh == 0xA:
                return 8, "stc.w", ["CCR", abs24(m.l(pc + 4))], 0
            return ILLEGAL
        if op == 0x78 and not b1 & 0x8F:
            w2 = m.w(pc + 4)
            if w2 >> 8 == 0x6B and (w2 & 0xFF) in (0x20, 0xA0):
                d = f"DISP:{(b1 >> 4) & 7}:{hx(m.l(pc + 6))}"
                return (10, "ldc.w", [d, "CCR"], 0) if (w2 & 0xFF) == 0x20 else (10, "stc.w", ["CCR", d], 0)
        return ILLEGAL
    if lo == 0xC0 and op in (0x50, 0x52):
        if op == 0x50:
            return 4, "mulxs.b", [r8(bh), r16(bl)], 0
        return (4, "mulxs.w", [r16(bh), r32(bl)], 0) if not bl & 8 else ILLEGAL
    if lo == 0xD0 and op in (0x51, 0x53):
        if op == 0x51:
            return 4, "divxs.b", [r8(bh), r16(bl)], 0
        return (4, "divxs.w", [r16(bh), r32(bl)], 0) if not bl & 8 else ILLEGAL
    if lo == 0xF0 and op in (0x64, 0x65, 0x66):
        if bh & 8 or bl & 8:
            return ILLEGAL
        return 4, {0x64: "or.l", 0x65: "xor.l", 0x66: "and.l"}[op], [r32(bh), r32(bl)], 0
    return ILLEGAL


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    raw = open(args[0], "rb").read()
    rom_size = 0x40000
    entries: list[int] = []
    seeds: list[int] = []
    out_path = None
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--rom-size":
            rom_size = int(args[i + 1], 0); i += 2; continue
        if a == "--entry":
            entries.append(int(args[i + 1], 0)); i += 2; continue
        if a == "--entries":
            for line in open(args[i + 1]):
                line = line.strip()
                if line:
                    seeds.append(int(line, 16))
            i += 2
            continue
        if a == "-o":
            out_path = args[i + 1]; i += 2; continue
        if a == "--ir":   # accepted for compatibility with reference/h8dasm
            i += 1
            continue
        print(f"unknown argument {a}", file=sys.stderr)
        return 2
    n = min(len(raw), rom_size)
    data = bytearray(n)
    data[0:n - (n & 1):2] = raw[1:n - (n & 1) + 1:2]
    data[1:n - (n & 1):2] = raw[0:n - (n & 1):2]
    m = Mem(bytes(data))
    funcs: set[int] = set()
    work: list[int] = []
    for v in range(64):
        t = m.l(v * 4) & 0xFFFFFF
        if t and t < n and t >= 0x100 and not t & 1:
            funcs.add(t)
            work.append(t)
    for e in entries:
        funcs.add(e)
        work.append(e)
    work += [s for s in seeds if s < n]
    insns: dict[int, tuple] = {}
    while work:
        pc = work.pop()
        while pc < n and pc not in insns:
            ins = decode(m, pc)
            insns[pc] = ins
            ln, mn, ops, _ = ins
            is_call = mn in ("jsr", "bsr")
            is_jump = mn in ("bra", "jmp") or mn[0] == "b"
            memop = any(o.split(":")[0] in ("IND", "PRE", "POST", "DISP", "ABS", "MIND") for o in ops)
            t = None
            if mn in ("jsr", "jmp"):
                # reference/h8dasm's rules: @aa:24 is followed like a branch target; @@aa:8 names a
                # vector slot, followed when even; @ERn names nothing
                for o in ops:
                    if o.startswith("TGT:"):
                        t = int(o[4:], 16)
                    elif o.startswith("MIND:"):
                        v = int(o[5:], 16)
                        t = v if not v & 1 else None
            elif (is_call or is_jump) and not memop:
                for o in ops:
                    if o.startswith("TGT:"):
                        t = int(o[4:], 16)   # bsr / Bcc: the relative target
            if t is not None and t < n:
                work.append(t)
                if is_call:
                    funcs.add(t)
            if mn in ("rts", "rte", "bra", "jmp", "sleep"):
                break
            pc += ln
    lines = []
    for pc in sorted(insns):
        ln, mn, ops, fl = insns[pc]
        lines.append("\t".join([f"{pc:06x}", str(ln), f"{fl:x}", mn] + ops))
    lines += [f"FUNC\t{f:06x}" for f in sorted(funcs)]
    text = "\n".join(lines) + "\n"
    if out_path:
        with open(out_path, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
