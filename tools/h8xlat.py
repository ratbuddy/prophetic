#!/usr/bin/env python3
"""H8/300H -> C++ translator for the Prophecy control firmware (bring-your-own-ROM).

Usage: py tools/h8xlat.py <ic22.ir> <out.cpp> [entries.txt ...]
(entries files: one hex address per line, for functions reached only indirectly, e.g. the
coefficient compute functions and the parameter-handler table)

Input: structured IR from `tools/h8dec.py` (or `reference/h8dasm --ir`: the same) (pc, length, flow flags, mnemonic, typed
operands; FUNC lines for entry points). Output: one C++ function per H8 function over the runtime
in engine/src/control/h8rt.h, plus the dispatch table. The output is derived from the user's ROM:
write it under generated/ (gitignored), never commit it.

Semantics follow the H8/300H programming manual: operand order "src, dst"; 8/16-bit ops touch only
the addressed register part; N/Z/V/C per instruction class (H is not modelled: no DAA/DAS).
"""
from __future__ import annotations

import sys
from pathlib import Path

COND = {
    "bt": "true", "bra": "true", "bf": "false", "brn": "false",
    "bhi": "!(c.C || c.Z)", "bls": "(c.C || c.Z)", "bcc": "!c.C", "bhs": "!c.C", "bcs": "c.C", "blo": "c.C",
    "bne": "!c.Z", "beq": "c.Z", "bvc": "!c.V", "bvs": "c.V", "bpl": "!c.N", "bmi": "c.N",
    "bge": "(c.N == c.V)", "blt": "(c.N != c.V)", "bgt": "(!c.Z && c.N == c.V)", "ble": "(c.Z || c.N != c.V)",
}
TERMINATORS = {"rts", "rte", "jmp", "bt", "bra", "sleep", "illegal", "trapa"}
BITS = {"b": 8, "w": 16, "l": 32}


class Unsupported(Exception):
    pass


def parse_ir(path: Path):
    insns, funcs = {}, set()
    for line in path.read_text().splitlines():
        p = line.split("\t")
        if p[0] == "FUNC":
            funcs.add(int(p[1], 16))
            continue
        insns[int(p[0], 16)] = (int(p[1]), int(p[2], 16), p[3], p[4:])
    return insns, funcs


def split(op: str):
    k, _, v = op.partition(":")
    return k, v


class Fn:
    """Emits one translated function."""

    def __init__(self, entry: int, insns: dict, entries: set):
        self.entry, self.insns, self.entries = entry, insns, entries
        self.body = self.reach()

    def reach(self):
        body, work = set(), [self.entry]
        while work:
            pc = work.pop()
            while pc not in body:
                if pc not in self.insns:
                    body.add(pc)
                    break
                body.add(pc)
                ln, _fl, mn, ops = self.insns[pc]
                base = mn.split(".")[0]
                if base in COND and ops and split(ops[0])[0] == "TGT":
                    work.append(int(split(ops[0])[1], 16))
                if base == "jmp" and ops and split(ops[0])[0] == "TGT":
                    t = int(split(ops[0])[1], 16)
                    if t not in self.entries:
                        work.append(t)
                if base in TERMINATORS or ln == 0:
                    break
                pc += ln
        return body

    # ---- operand access -------------------------------------------------------------------
    @staticmethod
    def regr(op, bits):
        k, v = split(op)
        if k == "R8": return f"u32(c.r8({v}))"
        if k == "R16": return f"u32(c.r16({v}))"
        if k == "R32": return f"c.er[{v}]"
        raise Unsupported(op)

    @staticmethod
    def regw(op, expr):
        k, v = split(op)
        if k == "R8": return f"c.w8({v}, u8({expr}));"
        if k == "R16": return f"c.w16({v}, u16({expr}));"
        if k == "R32": return f"c.er[{v}] = u32({expr});"
        raise Unsupported(op)

    @staticmethod
    def ea(op, bits, var="ea"):
        """Code computing the effective address into `var` (applying pre-dec/post-inc)."""
        k, v = split(op)
        n = bits // 8
        if k == "IND": return f"const u32 {var} = c.er[{v}] & 0xffffff;"
        if k == "PRE": return f"c.er[{v}] -= {n}; const u32 {var} = c.er[{v}] & 0xffffff;"
        if k == "POST": return f"const u32 {var} = c.er[{v}] & 0xffffff; c.er[{v}] += {n};"
        if k == "DISP":
            r, d = v.split(":")
            return f"const u32 {var} = (c.er[{r}] + 0x{d}u) & 0xffffff;"
        if k == "ABS": return f"const u32 {var} = 0x{v}u;"
        raise Unsupported(op)

    @staticmethod
    def is_mem(op):
        return split(op)[0] in ("IND", "PRE", "POST", "DISP", "ABS")

    def read(self, op, bits, var):
        """(setup code, expression) reading operand `op` of size bits."""
        k, v = split(op)
        if k == "IMM": return "", f"0x{v}u"
        if k in ("R8", "R16", "R32"): return "", self.regr(op, bits)
        if self.is_mem(op):
            return self.ea(op, bits, var), f"u32(c.m->rd{bits}({var}))"
        raise Unsupported(op)

    def write(self, op, bits, expr, var):
        k, v = split(op)
        if k in ("R8", "R16", "R32"): return "", self.regw(op, expr)
        if self.is_mem(op):
            return self.ea(op, bits, var), f"c.m->wr{bits}({var}, u{bits}({expr}));"
        raise Unsupported(op)

    def target(self, t: int, nxt: int, kind: str) -> str:
        if kind == "call":
            return f"call(c, 0x{t:06x}u, 0x{nxt:06x}u); if (c.unwind) return;"
        if t in self.body and t not in self.entries:
            if t <= self.cur:   # backward branch = loop: guard against hardware-poll spins
                return f"if (c.spin(0x{self.cur:06x}u)) return; goto L_{t:06x};"
            return f"goto L_{t:06x};"
        return f"tail(c, 0x{t:06x}u); return;"

    # ---- instructions ---------------------------------------------------------------------
    def insn(self, pc: int) -> str:
        if pc not in self.insns:
            return f"c.hooks->unsupported(0x{pc:06x}u, \"undecoded\"); return;"
        ln, _fl, mn, ops = self.insns[pc]
        nxt = pc + ln
        self.cur = pc
        base, _, suf = mn.partition(".")
        bits = BITS.get(suf, 8)
        try:
            return self.sem(pc, nxt, mn, base, suf, bits, ops)
        except Unsupported as e:
            return f"c.hooks->unsupported(0x{pc:06x}u, \"{mn} {e}\"); return;"

    def sem(self, pc, nxt, mn, base, suf, bits, ops):
        if base in COND:
            k, v = split(ops[0])
            cond = COND[base]
            if k != "TGT":
                raise Unsupported(ops[0])
            go = self.target(int(v, 16), nxt, "jump")
            return go if cond == "true" else ("" if cond == "false" else f"if ({cond}) {{ {go} }}")
        if base in ("bsr", "jsr"):
            k, v = split(ops[0])
            if k == "TGT": return self.target(int(v, 16), nxt, "call")
            if k == "R32": return f"call(c, c.er[{v}] & 0xffffff, 0x{nxt:06x}u); if (c.unwind) return;"
            if k == "MIND": return f"call(c, c.m->rd32(0x{v}u) & 0xffffff, 0x{nxt:06x}u); if (c.unwind) return;"
            raise Unsupported(ops[0])
        if base == "jmp":
            k, v = split(ops[0])
            if k == "TGT": return self.target(int(v, 16), nxt, "jump")
            if k == "R32": return f"tail(c, c.er[{v}] & 0xffffff); return;"
            if k == "MIND": return f"tail(c, c.m->rd32(0x{v}u) & 0xffffff); return;"
            raise Unsupported(ops[0])
        if base == "rts": return "c.pop32(); return;"
        if base == "rte": return "{ const u32 v = c.pop32(); c.set_ccr(u8(v >> 24)); return; }"
        if base == "nop": return ";"
        if base in ("sleep", "illegal", "trapa"):
            raise Unsupported("control-flow")

        if base == "mov":
            s, d = ops
            rs, ve = self.read(s, bits, "ea_s")
            ws, wv = self.write(d, bits, "r", "ea_d")
            return f"{{ {rs} const u32 r = c.mov({ve}, {bits}); {ws} {wv} }}"
        if base in ("add", "sub", "cmp"):
            s, d = ops
            rs, ve = self.read(s, bits, "ea_s")
            op = "add" if base == "add" else "sub"
            st = f"{{ {rs} const u32 r = c.{op}({self.regr(d, bits)}, {ve}, {bits});"
            return st + ("" if base == "cmp" else " " + self.regw(d, "r")) + " }"
        if base in ("adds", "subs"):
            s, d = ops
            sign = "+" if base == "adds" else "-"
            return f"c.er[{split(d)[1]}] {sign}= 0x{split(s)[1]}u;"
        if base in ("inc", "dec"):
            n, d = ("1", ops[0]) if len(ops) == 1 else (split(ops[0])[1], ops[1])
            return f"{{ const u32 r = c.{base}({self.regr(d, bits)}, 0x{n}u, {bits}); {self.regw(d, 'r')} }}"
        if base in ("and", "or", "xor"):
            s, d = ops
            o = {"and": "&", "or": "|", "xor": "^"}[base]
            rs, ve = self.read(s, bits, "ea_s")
            return f"{{ {rs} const u32 r = c.logic({self.regr(d, bits)} {o} {ve}, {bits}); {self.regw(d, 'r')} }}"
        if base in ("andc", "orc", "xorc"):
            s, d = ops
            if split(d)[0] != "CCR":
                raise Unsupported(d)
            o = {"andc": "&", "orc": "|", "xorc": "^"}[base]
            return f"c.set_ccr(u8(c.ccr() {o} 0x{split(s)[1]}u));"
        if base == "not":
            d = ops[0]
            return f"{{ const u32 r = c.logic(~{self.regr(d, bits)}, {bits}); {self.regw(d, 'r')} }}"
        if base == "neg":
            d = ops[0]
            return f"{{ const u32 r = c.sub(0, {self.regr(d, bits)}, {bits}); {self.regw(d, 'r')} }}"
        if base == "extu":
            d = ops[0]
            frm = 8 if suf == "w" else 16
            return f"{{ const u32 r = c.mov({self.regr(d, bits)} & 0x{(1 << frm) - 1:x}u, {bits}); {self.regw(d, 'r')} }}"
        if base == "exts":
            d = ops[0]
            frm = 8 if suf == "w" else 16
            return f"{{ const u32 r = c.exts({self.regr(d, bits)}, {frm}, {bits}); {self.regw(d, 'r')} }}"
        if base in ("shll", "shal", "shlr", "shar", "rotl", "rotr", "rotxl", "rotxr"):
            d = ops[0]
            return f"{{ const u32 r = c.{base}({self.regr(d, bits)}, {bits}); {self.regw(d, 'r')} }}"
        if base in ("mulxu", "mulxs"):
            s, d = ops
            if suf == "b":
                a, b = f"u8(c.r16({split(d)[1]}))", f"u8({self.regr(s, 8)})"
                if base == "mulxu":
                    return f"c.w16({split(d)[1]}, u16(u32({a}) * u32({b})));"
                return f"{{ const u16 r = u16(s32(s8({a})) * s8({b})); c.w16({split(d)[1]}, r); c.nz(r, 16); }}"
            a, b = f"u16(c.er[{split(d)[1]}])", f"u16({self.regr(s, 16)})"
            if base == "mulxu":
                return f"c.er[{split(d)[1]}] = u32({a}) * u32({b});"
            return f"{{ const u32 r = u32(s32(s16({a})) * s16({b})); c.er[{split(d)[1]}] = r; c.nz(r, 32); }}"
        if base in ("divxu", "divxs"):
            s, d = ops
            dn = split(d)[1]
            if suf == "b" and base == "divxu":
                return (f"{{ const u32 dv = u8({self.regr(s, 8)}); const u32 dd = c.r16({dn}); c.N = (dv >> 7) & 1; c.Z = dv == 0;"
                        f" if (dv) c.w16({dn}, u16(((dd % dv) << 8) | ((dd / dv) & 0xff))); }}")
            if suf == "w" and base == "divxu":
                return (f"{{ const u32 dv = u16({self.regr(s, 16)}); const u32 dd = c.er[{dn}]; c.N = (dv >> 15) & 1; c.Z = dv == 0;"
                        f" if (dv) c.er[{dn}] = ((dd % dv) << 16) | ((dd / dv) & 0xffff); }}")
            if suf == "w" and base == "divxs":
                return (f"{{ const s32 dv = s16(u16({self.regr(s, 16)})); const s32 dd = s32(c.er[{dn}]); c.Z = dv == 0;"
                        f" if (dv) {{ const s32 q = dd / dv, r = dd % dv; c.er[{dn}] = (u32(u16(r)) << 16) | u16(q); c.N = q < 0; }} }}")
            raise Unsupported(mn)
        if base in ("bset", "bclr", "bnot", "btst"):
            b, t = ops
            bk, bv = split(b)
            bit = bv if bk == "BIT" else f"(c.r8({bv}) & 7)"
            rs, ve = self.read(t, 8, "ea_t")
            op = {"bset": f"v | (1u << {bit})", "bclr": f"v & ~(1u << {bit})", "bnot": f"v ^ (1u << {bit})"}
            if base == "btst":
                return f"{{ {rs} const u32 v = {ve}; c.Z = !((v >> {bit}) & 1); }}"
            if self.is_mem(t):
                k, v = split(t)
                return f"{{ {rs} const u32 v = {ve}; c.m->wr8(ea_t, u8({op[base]})); }}"
            return f"{{ const u32 v = {ve}; {self.regw(t, op[base])} }}"
        if base == "eepmov":
            return ("while (u8(c.er[4]) != 0) { c.m->wr8(c.er[6] & 0xffffff, c.m->rd8(c.er[5] & 0xffffff)); "
                    "c.er[5]++; c.er[6]++; c.w8(12, u8(c.r8(12) - 1)); }")
        if base == "stc":
            s, d = ops
            if suf == "w":
                ws, wv = self.write(d, 16, "u32(c.ccr()) << 8", "ea_d")
                return f"{{ {ws} {wv} }}"
            return self.regw(d, "c.ccr()")
        if base == "ldc":
            s, d = ops
            if suf == "w":
                rs, ve = self.read(s, 16, "ea_s")
                return f"{{ {rs} c.set_ccr(u8({ve} >> 8)); }}"
            rs, ve = self.read(s, 8, "ea_s")
            return f"{{ {rs} c.set_ccr(u8({ve})); }}"
        raise Unsupported(mn)

    def emit(self) -> str:
        lines = [f"static void f_{self.entry:06x}(Cpu &c)", "{", f"\tgoto L_{self.entry:06x};"]
        for pc in sorted(self.body):
            lines.append(f"L_{pc:06x}: {self.insn(pc)}")
        lines.append("}")
        return "\n".join(lines)


def main() -> int:
    insns, funcs = parse_ir(Path(sys.argv[1]))
    out = Path(sys.argv[2])
    for extra in sys.argv[3:]:
        funcs |= {int(x, 16) for x in Path(extra).read_text().split() if x}
    # every direct call target is an entry too
    for pc, (ln, fl, mn, ops) in insns.items():
        if mn.split(".")[0] in ("jsr", "bsr") and ops and split(ops[0])[0] == "TGT":
            funcs.add(int(split(ops[0])[1], 16))
    funcs = sorted(f for f in funcs if f in insns)
    entries = set(funcs)
    fns = [Fn(f, insns, entries) for f in funcs]
    unsupported = sum(f.emit().count("unsupported(") for f in fns)
    src = [
        "// GENERATED by tools/h8xlat.py from the user's H8 ROM - firmware-derived, do not commit.",
        "#include \"control/h8rt.h\"",
        "",
        "#if defined(__GNUC__)",
        "#pragma GCC diagnostic ignored \"-Wunused-label\"",
        "#endif",
        "",
        "namespace kp::h8 {",
        "",
    ]
    src += [f"static void f_{f.entry:06x}(Cpu &c);" for f in fns]
    src += [""] + [f.emit() + "\n" for f in fns]
    src += ["Fn translated(u32 a)", "{", "\tswitch (a)", "\t{"]
    src += [f"\tcase 0x{f.entry:06x}u: return f_{f.entry:06x};" for f in fns]
    src += ["\tdefault: return nullptr;", "\t}", "}", "",
            "void call(Cpu &c, u32 a, u32 ret)",
            "{",
            "\tc.push32(ret);",
            "\tc.calls++;",
            "\tif (c.hooks && c.hooks->call(a, c)) { c.pop32(); return; }",
            "\tif (Fn f = translated(a)) { f(c); return; }",
            "\tif (c.hooks) c.hooks->unsupported(a, \"call to untranslated\");",
            "\tc.pop32();",
            "}", "",
            "void tail(Cpu &c, u32 a)",
            "{",
            "\tif (c.hooks && c.hooks->call(a, c)) { c.pop32(); return; }",
            "\tif (Fn f = translated(a)) { f(c); return; }",
            "\tif (c.hooks) c.hooks->unsupported(a, \"tail to untranslated\");",
            "\tc.pop32();",
            "}", "", "} // namespace kp::h8", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(src))
    print(f"h8xlat: {len(fns)} functions, {sum(len(f.body) for f in fns)} translated instructions, "
          f"{unsupported} unsupported sites -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
