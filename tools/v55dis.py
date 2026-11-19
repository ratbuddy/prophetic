#!/usr/bin/env python3
"""Disassemble part of the V55 (ic12) ROM with capstone's 8086 mode (the V55 runs 8086 code; NEC
mnemonics differ: prepare/dispose = enter/leave, br = jmp, cvtbw = cbw). Reads your own dump.
Usage: py tools/v55dis.py <start> <end> [ic12.bin]   (physical addresses, hex: the ROM is at 0x80000)
"""
import sys

import capstone

rom = sys.argv[3] if len(sys.argv) > 3 else "data/roms/korgpro20/ic12_v20.bin"
d = open(rom, "rb").read()
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
seg = start & 0xF0000   # near jumps/calls are shown relative to this 64 KiB segment
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
for ins in md.disasm(d[start - 0x80000:end - 0x80000], start - seg):
    print("%05X: %-8s %s" % (seg + ins.address, ins.mnemonic, ins.op_str))
