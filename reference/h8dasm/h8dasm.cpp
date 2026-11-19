// Recursive-descent H8/300H disassembler for the Prophecy's H8 ROM (ic22), built on MAME's BSD-3
// h8h_disassembler. Dev/analysis tool only: its output is firmware-derived and goes under data/.
//
// Usage: h8dasm <ic22.bin> [--rom-size 0x40000] [--entry addr]... > listing.txt
// The dump is word-swapped relative to the CPU (MAME ROM_LOAD16_WORD_SWAP); this tool unswaps it.
// Flow: starts at the reset/interrupt vectors (32-bit advanced-mode table, 64 entries) plus any
// --entry, follows bcc/bra/jmp/jsr/bsr targets that are immediate, and prints:
//   FUNC <addr> callers=<n>        for every jsr/bsr target and vector
//   <addr>: <bytes> <mnemonic>     for every reached instruction, sorted, with block labels
#include "emu.h"
#include "cpu/h8/h8hd.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Structured operand decoder over MAME's decode table: same entry match as
// h8_disassembler::disassemble(), but each operand is emitted as KIND:value instead of text.
class ir_disassembler : public h8h_disassembler
{
public:
	std::string ir(offs_t pc, const data_buffer &op, u32 &len)
	{
		u32 slot[5];
		slot[0] = op.r16(pc);
		slot[1] = op.r32(pc);
		slot[2] = (op.r16(pc) << 16) | op.r16(pc + 4);
		slot[3] = (op.r16(pc) << 16) | op.r16(pc + 6);
		slot[4] = op.r32(pc + 2);
		int inst;
		for (inst = 0;; inst++)
		{
			const disasm_entry &e = m_table[inst];
			if ((slot[e.m_slot] & e.m_mask) == e.m_val && (slot[0] & e.m_mask0) == e.m_val0)
				break;
		}
		const disasm_entry &e = m_table[inst];
		len = e.m_flags & LENGTHMASK;
		std::string s = std::string(e.m_opcode);
		for (int am : { e.m_am1, e.m_am2 })
			if (am != DASM_none)
				s += "\t" + operand(am, pc, op, slot[e.m_slot], e.m_slot, len);
		return s;
	}

private:
	static std::string hx(u32 v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return b; }
	std::string operand(int am, offs_t pc, const data_buffer &op, u32 opcode, int slot, int offset)
	{
		const offs_t epc = pc + offset;
		const u32 M = 0xfffff;   // 1 MiB mode: A20 wraps
		switch (am)
		{
		case DASM_r8l: return "R8:" + std::to_string(opcode & 15);
		case DASM_r8h: return "R8:" + std::to_string((opcode >> 4) & 15);
		case DASM_r8u: return "R8:" + std::to_string((opcode >> 8) & 15);
		case DASM_r16l: return "R16:" + std::to_string(opcode & 15);
		case DASM_r16h: return "R16:" + std::to_string((opcode >> 4) & 15);
		case DASM_r32l: return "R32:" + std::to_string(opcode & 7);
		case DASM_r32h: return "R32:" + std::to_string((opcode >> 4) & 7);
		case DASM_r32ih: return "IND:" + std::to_string((opcode >> 4) & 7);
		case DASM_r32ihh: return "IND:" + std::to_string((opcode >> 20) & 7);
		case DASM_pr32h: return "PRE:" + std::to_string((opcode >> 4) & 7);
		case DASM_r32pl: return "POST:" + std::to_string(opcode & 7);
		case DASM_r32ph: return "POST:" + std::to_string((opcode >> 4) & 7);
		case DASM_r32d16h: return "DISP:" + std::to_string((opcode >> 4) & 7) + ":" + hx(u32(s32(s16(op.r16(epc - 2)))));
		case DASM_r32d32hh: return "DISP:" + std::to_string((opcode >> 20) & 7) + ":" + hx(op.r32(epc - 4));
		case DASM_psp: return "PRE:7";
		case DASM_spp: return "POST:7";
		case DASM_r32n2l: return "RLIST:" + std::to_string(opcode & 6) + ":2";
		case DASM_r32n3l: return "RLIST:" + std::to_string(opcode & 4) + ":3";
		case DASM_r32n4l: return "RLIST:" + std::to_string(opcode & 4) + ":4";
		case DASM_abs8: return "ABS:" + hx((0xffff00 | op.r8(pc + 1)) & M);
		case DASM_abs16: return "ABS:" + hx(u32(s32(s16(op.r16(slot == 2 ? epc - 4 : epc - 2)))) & M);
		case DASM_abs32: return "ABS:" + hx(op.r32(slot == 3 ? epc - 6 : epc - 4) & M);
		case DASM_abs8i: return "MIND:" + hx(op.r8(pc + 1));
		case DASM_abs16e: return "TGT:" + hx(op.r16(pc + 2));
		case DASM_abs24e: return "TGT:" + hx(op.r32(pc) & 0xffffff);
		case DASM_rel8: return "TGT:" + hx((pc + 2 + s8(op.r8(pc + 1))) & 0xffffff);
		case DASM_rel16: return "TGT:" + hx((pc + 4 + s16(op.r16(pc + 2))) & 0xffffff);
		case DASM_one: return "IMM:1";
		case DASM_two: return "IMM:2";
		case DASM_four: return "IMM:4";
		case DASM_imm2: return "IMM:" + hx((opcode >> 4) & 3);
		case DASM_imm3: return "BIT:" + hx((opcode >> 4) & 7);
		case DASM_imm8: return "IMM:" + hx(op.r8(epc - 1));
		case DASM_imm16: return "IMM:" + hx(op.r16(pc + 2));
		case DASM_imm32: return "IMM:" + hx(op.r32(pc + 2));
		case DASM_ccr: return "CCR";
		case DASM_exr: return "EXR";
		}
		return "UNK:" + std::to_string(am);
	}
};

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <ic22.bin> [--rom-size N] [--entry ADDR]...\n", argv[0]);
		return 2;
	}
	std::ifstream f(argv[1], std::ios::binary);
	std::vector<u8> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	u32 rom_size = 0x40000;   // korgprophecy h8_map: SRAM overlays 0x40000+
	std::vector<u32> extra, seeds;
	bool ir_mode = false;
	for (int i = 2; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--ir") { ir_mode = true; continue; }
		if (a == "--rom-size" && i + 1 < argc) rom_size = u32(std::strtoul(argv[++i], nullptr, 0));
		else if (a == "--entry" && i + 1 < argc) extra.push_back(u32(std::strtoul(argv[++i], nullptr, 0)));
		else if (a == "--entries" && i + 1 < argc)
		{
			// one hex address per line (e.g. executed PCs from tools/h8trace_analyze.py); these seed
			// flow analysis but are not marked as functions
			std::ifstream ef(argv[++i]);
			std::string line;
			while (std::getline(ef, line))
				if (!line.empty())
					seeds.push_back(u32(std::strtoul(line.c_str(), nullptr, 16)));
		}
	}
	std::vector<u8> mem(std::min<size_t>(raw.size(), rom_size));
	for (size_t i = 0; i + 1 < mem.size(); i += 2)
	{
		mem[i] = raw[i + 1];
		mem[i + 1] = raw[i];
	}

	h8h_disassembler dis;
	util::disasm_interface::data_buffer buf(mem);
	std::set<u32> funcs, labels;
	std::map<u32, std::set<u32>> callers;
	std::map<u32, std::string> vec_name;
	std::vector<u32> work;
	for (u32 v = 0; v < 64; v++)
	{
		const u32 t = buf.r32(v * 4) & 0xffffff;
		if (t && t < mem.size() && t >= 0x100 && !(t & 1))
		{
			funcs.insert(t);
			vec_name[t] += (vec_name[t].empty() ? "" : ",") + std::string("vec") + std::to_string(v);
			work.push_back(t);
		}
	}
	for (u32 e : extra)
	{
		funcs.insert(e);
		work.push_back(e);
	}
	for (u32 s : seeds)
		if (s < mem.size())
			work.push_back(s);

	struct Insn { u32 len; std::string text; u32 flags; };
	std::map<u32, Insn> insns;
	const std::regex target_re("h'([0-9a-f]{4,6})");
	while (!work.empty())
	{
		u32 pc = work.back();
		work.pop_back();
		while (pc < mem.size() && !insns.count(pc))
		{
			std::ostringstream os;
			const u32 r = dis.disassemble(os, pc, buf, buf);
			const u32 len = r & util::disasm_interface::LENGTHMASK;
			const std::string text = os.str();
			insns[pc] = { len, text, r };
			if (len == 0)
				break;
			const std::string mn = text.substr(0, text.find(' '));
			const bool is_call = mn == "jsr" || mn == "bsr";
			const bool is_jump = mn == "bra" || mn == "jmp" || mn[0] == 'b';   // bcc/bra/bsr
			std::smatch m;
			if ((is_call || is_jump) && std::regex_search(text, m, target_re) && text.find('@') == std::string::npos)
			{
				const u32 t = u32(std::strtoul(m[1].str().c_str(), nullptr, 16));
				if (t < mem.size())
				{
					work.push_back(t);
					if (is_call) { funcs.insert(t); callers[t].insert(pc); }
					else labels.insert(t);
				}
			}
			else if ((mn == "jsr" || mn == "jmp") && text.find("@h'") != std::string::npos)
			{
				// absolute jsr/jmp printed as @h'xxxxxx
				const auto p = text.find("@h'");
				const u32 t = u32(std::strtoul(text.c_str() + p + 3, nullptr, 16));
				if (t < mem.size() && !(t & 1))
				{
					work.push_back(t);
					if (mn == "jsr") { funcs.insert(t); callers[t].insert(pc); }
					else labels.insert(t);
				}
			}
			if (mn == "rts" || mn == "rte" || mn == "bra" || mn == "jmp" || mn == "sleep")
				break;
			pc += len;
		}
	}

	if (ir_mode)
	{
		// IR: pc \t len \t flags \t mnemonic [\t op1 [\t op2]] for every reached instruction,
		// then FUNC lines (jsr/bsr targets and vectors) for the translator.
		ir_disassembler ird;
		for (const auto &kv : insns)
		{
			u32 len = 0;
			const std::string s = ird.ir(kv.first, buf, len);
			std::printf("%06x\t%u\t%x\t%s\n", kv.first, len, kv.second.flags & 0x70000, s.c_str());
		}
		for (u32 fn : funcs)
			std::printf("FUNC\t%06x\n", fn);
		return 0;
	}

	size_t code_bytes = 0;
	for (const auto &kv : insns) code_bytes += kv.second.len;
	std::printf("; h8dasm: %zu instructions, %zu code bytes, %zu functions\n", insns.size(), code_bytes, funcs.size());
	for (u32 fn : funcs)
		std::printf("FUNC %06x callers=%zu %s\n", fn, callers[fn].size(), vec_name.count(fn) ? vec_name[fn].c_str() : "");
	u32 prev_end = 0;
	for (const auto &kv : insns)
	{
		const u32 pc = kv.first;
		if (pc != prev_end) std::printf("\n");
		if (funcs.count(pc))
		{
			std::printf("\n;---- FUNC %06x %s callers:", pc, vec_name.count(pc) ? vec_name[pc].c_str() : "");
			int n = 0;
			for (u32 c : callers[pc]) if (n++ < 8) std::printf(" %06x", c);
			std::printf("\n");
		}
		else if (labels.count(pc))
			std::printf("L_%06x:\n", pc);
		std::printf("%06x: ", pc);
		for (u32 i = 0; i < 10; i += 2) std::printf(i < kv.second.len ? "%04x " : "     ", buf.r16(pc + i));
		std::printf(" %s\n", kv.second.text.c_str());
		prev_end = pc + kv.second.len;
	}
	return 0;
}
