// Stand-in for MAME's TMS57002 disassembler header: the standalone core never disassembles.
#pragma once
#include "emu.h"

class tms57002_disassembler : public util::disasm_interface
{
public:
	tms57002_disassembler() = default;
};
