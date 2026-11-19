#!/usr/bin/env python3
"""Register-mode TMS57002 op bodies for the ahead-of-time compiled DSP programs.

Usage: tms_regops.py <tms57002.hxx> <tms57002.cpp> <tms57002.h> <out.hxx>

Reads the interpreter's op functions (`ex_N`, the CINTRP section that tmsmake.py generates from
the vendored MAME tmsinstr.lst) and writes one macro per op, `KPR_N(P)`, with the same body but
the hot DSP state held in locals of the generated frame function instead of device members:

    promoted (locals): macc macc_read macc_write aacc creg ca id st1 sti     read-only: ba0
    everything else  : dv.<member> (dv = the device), D::<ENUM> for constants, i->param -> (P)

Helper member calls (macc_to_output_0s, write_dmem, ...) still read and write device members, so
around every top-level statement that calls one, the promoted state the helper reads or writes is
written to the device before it and the state it writes is read back after it. Read/write sets are
derived from the helper sources (recursively); a helper whose body is not found is assumed to touch
everything. get_cmem() becomes KP_CMEM(), whose common path (no coefficient update queued) works on
the locals. A statement where that bookkeeping cannot be exact (a helper writes state the statement
also uses) runs entirely on device memory: everything is written back before it and reloaded after.

The output is derived only from the vendored core (no firmware data). The A/B gate
engine/test/dsp_fast_test.cpp checks the compiled programs against the interpreter sample by sample.
"""
from __future__ import annotations

import re
import sys

PROMOTED = ["macc", "macc_read", "macc_write", "aacc", "ca", "id", "st1", "sti"]
READ_ONLY = {"ba0"}
BODY_LOCALS = {"c", "d", "r"}
KEEP = {"if", "else", "return", "int64_t", "int32_t", "uint64_t", "uint32_t", "uint16_t", "uint8_t", "int",
        "unsigned", "bool", "true", "false", "std", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
        "sizeof", "max", "min", "clamp"}
PURE_FREE = {"max", "min", "clamp", "BIT", "rounded_clipped_macc_output", "int64_t", "int32_t", "uint32_t",
             "uint64_t", "u32", "s64", "u64", "s32", "u8", "bool"}

# Helpers whose first statement is `if (!<knob>) return current;` (debug forwards): emitted as
# KP_MPY_D / KP_LMHD_D, which return the operand; dsp_core.cpp runs the interpreter whenever one of
# the knobs is set, so the compiled code never needs the helper.
DEBUG_FORWARD = {"mpy_d_operand": "KP_MPY_D", "lmhd_d_operand": "KP_LMHD_D"}
# Debug knobs the op bodies test (constructor default false, nothing sets them): emitted as macros
# that dsp_core.cpp defines false, so the dead path folds away; dsp_core.cpp runs the interpreter
# whenever one of them is set.
DEBUG_KNOBS = {"m_debug_smhd_raw_path": "KP_SMHD_RAW"}

TOKEN_RE = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*'
                      r'|[A-Za-z_]\w*|->|::|<<=|>>=|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|.', re.S)
ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}


def tokens(text: str) -> list[str]:
    return [t for t in TOKEN_RE.findall(text) if t.strip() and not t.startswith("//") and not t.startswith("/*")]


def balanced_body(text: str, open_idx: int) -> str:
    depth = 0
    for k in range(open_idx, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:k]
    raise ValueError("unbalanced braces")


class Helpers:
    """Promoted-state read/write sets of the core's member functions."""

    def __init__(self, cpp: str, hdr: str):
        self.cpp, self.hdr = cpp, hdr
        self.memo: dict[str, tuple[set, set]] = {}
        self.unknown: set[str] = set()

    def body(self, name: str) -> str | None:
        m = re.search(r"tms57002_device::" + re.escape(name) + r"\s*\([^;{]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", self.cpp)
        if m:
            return balanced_body(self.cpp, m.end() - 1)
        m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{()]*(?:\([^)]*\)[^;{()]*)*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", self.hdr)
        if m:
            return balanced_body(self.hdr, m.end() - 1)
        return None

    def rw(self, name: str, depth: int = 0) -> tuple[set, set]:
        if name in self.memo:
            return self.memo[name]
        b = self.body(name)
        if b is None:
            if name in PURE_FREE:
                return set(), set()
            self.unknown.add(name)
            res = (set(PROMOTED), set(PROMOTED))   # unknown: touches everything
            self.memo[name] = res
            return res
        self.memo[name] = (set(PROMOTED), set(PROMOTED))   # recursion guard: conservative
        t = tokens(b)
        r, w = set(), set()
        for k, tok in enumerate(t):
            prev = t[k - 1] if k else ""
            nxt = t[k + 1] if k + 1 < len(t) else ""
            if prev in (".", "->", "::"):
                continue
            if tok in PROMOTED:
                r.add(tok)
                if nxt in ASSIGN_OPS or nxt in ("++", "--") or prev in ("++", "--"):
                    w.add(tok)
            elif nxt == "(" and re.match(r"[A-Za-z_]\w*$", tok) and tok not in KEEP and depth < 8:
                if tok in ("if", "for", "while", "switch", "return", "sizeof"):
                    continue
                cr, cw = self.rw(tok, depth + 1)
                r |= cr
                w |= cw
        self.memo[name] = (r, w)
        return r, w


def parse_stmts(t: list[str], i: int, end: int) -> list[tuple[int, int]]:
    """Top-level statements of t[i:end] as (start, stop) token ranges."""
    out = []
    while i < end:
        j = stmt_end(t, i, end)
        out.append((i, j))
        i = j
    return out


def stmt_end(t: list[str], i: int, end: int) -> int:
    if t[i] == "{":
        depth = 0
        for k in range(i, end):
            if t[k] == "{":
                depth += 1
            elif t[k] == "}":
                depth -= 1
                if depth == 0:
                    return k + 1
        raise ValueError("unbalanced block")
    if t[i] == "if":
        depth = 0
        k = i + 1
        while True:
            if t[k] == "(":
                depth += 1
            elif t[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        k = stmt_end(t, k + 1, end)
        if k < end and t[k] == "else":
            k = stmt_end(t, k + 1, end)
        return k
    depth = 0
    for k in range(i, end):
        if t[k] in "([":
            depth += 1
        elif t[k] in ")]":
            depth -= 1
        elif t[k] == ";" and depth == 0:
            return k + 1
    raise ValueError("statement without ';'")


def emit_stmt(t: list[str], helpers: Helpers) -> tuple[str, str]:
    """Returns (code, mode) for one top-level statement."""
    calls = []
    sp = set()
    uses_cmem = False
    for k, tok in enumerate(t):
        prev = t[k - 1] if k else ""
        nxt = t[k + 1] if k + 1 < len(t) else ""
        if prev in (".", "->", "::"):
            continue
        if tok in PROMOTED:
            sp.add(tok)
        if nxt == "(" and re.match(r"[A-Za-z_]\w*$", tok) and tok not in KEEP and tok not in BODY_LOCALS:
            if tok == "get_cmem":
                uses_cmem = True
            elif tok not in DEBUG_FORWARD:
                calls.append(tok)
    if uses_cmem:
        sp |= {"sti", "st1"}
    hr, hw = set(), set()
    for c in calls:
        r, w = helpers.rw(c)
        hr |= r
        hw |= w
    register = not (hw & sp)
    out = []
    k = 0
    while k < len(t):
        tok = t[k]
        prev = t[k - 1] if k else ""
        nxt = t[k + 1] if k + 1 < len(t) else ""
        if tok == "i" and nxt == "->" and k + 2 < len(t) and t[k + 2] == "param":
            out.append("(P)")
            k += 3
            continue
        if prev in (".", "->", "::") or not re.match(r"[A-Za-z_]\w*$", tok):
            out.append(tok)
        elif tok == "return":
            out.append("break")   # bodies are wrapped in do { } while (0) and contain no loops
        elif tok in KEEP or tok in BODY_LOCALS or tok in READ_ONLY:
            out.append(tok)
        elif tok in PROMOTED:
            out.append(tok if register else "dv." + tok)
        elif tok == "get_cmem" and nxt == "(" and register:
            out.append("KP_CMEM")
        elif tok in DEBUG_FORWARD and nxt == "(":
            out.append(DEBUG_FORWARD[tok])   # macro: returns its first argument unless the debug knob is on
        elif tok in DEBUG_KNOBS:
            out.append(DEBUG_KNOBS[tok])     # macro: false (the compiled code never runs with the knob on)
        elif re.match(r"[A-Z][A-Z0-9_]*$", tok):
            out.append("D::" + tok)
        else:
            out.append("dv." + tok)
        k += 1
    code = " ".join(out)
    if register:
        pre = "".join("dv.%s = %s; " % (v, v) for v in PROMOTED if v in (hr | hw))
        post = "".join(" %s = dv.%s;" % (v, v) for v in PROMOTED if v in hw)
        return pre + code + post, "register"
    return "KP_FLUSH_ALL(); " + code + " KP_RELOAD_ALL();", "memory"


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    # tmsmake.py writes the .hxx in the platform encoding (cp1252 on Windows); only ASCII op bodies
    # are used, so decode leniently.
    hxx, cpp, hdr, out_path = (open(p, encoding="utf-8", errors="replace").read() if k < 3 else p
                               for k, p in enumerate(sys.argv[1:]))
    sec = re.search(r"#ifdef CINTRP\n(.*?)\n#endif", hxx, re.S)
    if not sec:
        print("no CINTRP section in " + sys.argv[1], file=sys.stderr)
        return 1
    helpers = Helpers(cpp, hdr)
    lines = ["// Generated by tools/tms_regops.py from the vendored TMS57002 core (no firmware data).",
             "// Register-mode op bodies for engine/src/dsp_core.cpp; see the script for the rules.", ""]
    counts = {"register": 0, "memory": 0}
    nops = 0
    for m in re.finditer(r"void tms57002_device::ex_(\d+)\(const icd \*i\) //([^\n]*)\n\{\n(.*?)\n\}\n", sec.group(1), re.S):
        n, comment, body = int(m.group(1)), m.group(2).strip(), m.group(3)
        t = tokens(body)
        # drop the standard local declarations "uint32_t c; (void)c;" etc.
        k = 0
        while k + 7 < len(t) and t[k] in ("uint32_t", "int64_t") and t[k + 1] in BODY_LOCALS and t[k + 2] == ";" \
                and t[k + 3] == "(" and t[k + 4] == "void" and t[k + 5] == ")" and t[k + 6] == t[k + 1] and t[k + 7] == ";":
            k += 8
        t = t[k:]
        stmts = []
        for a, b in parse_stmts(t, 0, len(t)):
            code, mode = emit_stmt(t[a:b], helpers)
            counts[mode] += 1
            stmts.append(code)
        lines.append("// %d: %s" % (n, comment))
        lines.append("#define KPR_%d(P) do { uint32_t c; (void)c; uint32_t d; (void)d; int64_t r; (void)r; \\" % n)
        for s in stmts:
            lines.append("\t" + s + " \\")
        lines.append("} while (0)")
        nops += 1
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s: %d ops, %d register-mode statements, %d memory-mode statements%s" % (
        out_path, nops, counts["register"], counts["memory"],
        "; helpers assumed to touch everything: " + ", ".join(sorted(helpers.unknown)) if helpers.unknown else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
