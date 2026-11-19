// The vendored TMS57002 core (unmodified) compiled as ONE translation unit, plus the runtime for
// the ahead-of-time compiled DSP programs (dsp_fast.h).
//
// One TU so that the generated program code can inline the core's op functions (ex_N, in
// tmsops.cpp) and their helpers (get_cmem, write_dmem, ... in tms57002.cpp). Every member is
// made public here, the same way prophecy_board.cpp sees the core, so member access (which MSVC
// encodes in symbol names) is consistent across all translation units. Build exactly one of:
// this file, or the three core .cpp files separately (engine/Makefile's libtms57002core.a).
#include "dsp_fast.h"

#include "emu.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// KP_DSP_PRISTINE: the vendored core exactly as is (no inline fast paths, no compiled programs);
// the reference build for engine/test/synth_render.cpp.
#ifdef KP_DSP_PRISTINE
#define KP_DSP_AOT_NONE
#endif

#define private public
#define protected public
#include "tms57002.h"
#include "tms57002.cpp"
#include "tms57kdec.cpp"
#ifndef KP_DSP_PRISTINE
// Inline fast path of get_cmem() for the op bodies in tmsops.cpp. With no coefficient update queued
// (update_counter_count == 0) get_cmem() does exactly `sti &= ~S_UPDATE; return current_cmem_view(addr);`
// (both update branches need a queued update). Every call site passes `ca` or `i->param` (no side
// effects) and no op expression also reads sti, so expanding it in place is equivalent. The
// self-reference inside the macro is not re-expanded: it calls the real function.
#define get_cmem(a) (update_counter_count == 0 ? (sti &= ~S_UPDATE, current_cmem_view(a)) : get_cmem(a))
#endif
#include "tmsops.cpp"
#undef get_cmem
#undef private
#undef protected

namespace kp {

using D = tms57002_device;

// One generated program: its decode-cache steps (one per hash node: pc + ST1 decode key) and
// the exact op chain each was compiled from, plus the compiled frame function.
struct AotStep
{
	std::uint8_t pc;
	std::uint32_t key;      // st1 & ST1_CACHE at decode time
	std::uint16_t op_first; // index into ops
	std::uint8_t op_count;  // ops including the terminator (op < 4, last)
	std::int16_t succ;      // chained next step (terminator next), -1 = lookup
};
struct AotOp
{
	std::uint16_t op;
	std::uint8_t param;
};
struct AotProgram
{
	std::uint64_t hash;     // D::jit_program_hash() of the program words
	const AotStep *steps;
	int step_count;
	const AotOp *ops;
	void (*run)(D &d, DspFast &f);
};

#if defined(_MSC_VER)
#define KP_NOINLINE __declspec(noinline)
#define KP_UNLIKELY(x) (x)
#define KP_LIKELY(x) (x)
#else
#define KP_NOINLINE __attribute__((noinline))
#define KP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define KP_LIKELY(x) __builtin_expect(!!(x), 1)
#endif
// The rarely taken calls every compiled step has (xm_step, cmem_slow, pc0_changed): with the
// standard convention each call site clobbers the caller-saved registers, so the allocator keeps
// the frame function's hot state out of them or spills around the site, on the hot path too.
// preserve_most makes the callee save nearly every register instead (arm64: all but x0-x8 and
// x16-x18), so the hot code keeps its registers. Clang only (GCC and MSVC have no equivalent).
#if defined(__clang__) && (defined(__aarch64__) || defined(__x86_64__))
#define KP_COLD_CC __attribute__((preserve_most))
#else
#define KP_COLD_CC
#endif

namespace {

bool g_enabled = true;
int g_budget_cap = 0;
bool g_stream_raw = true;

enum { kNext, kRepeat, kLookup, kStop };

// The interpreter's per-instruction epilogue (execute_run, after the op chain), for the cases the
// generated fast path does not take inline: pending pre-transfer, repeat, branch, rptc_next.
// Returns where execution continues.
[[maybe_unused]] KP_NOINLINE int slow_post(D &d)
{
	if (d.m_pending_pre_transfer != D::pending_pre_transfer_type::none)
		d.apply_pending_pre_transfer();
	d.icount--;
	int r;
	if (d.rptc)
	{
		d.rptc--;
		r = kRepeat;
	}
	else if (d.sti & D::S_BRANCH)
	{
		d.sti &= ~D::S_BRANCH;
		r = kLookup;
	}
	else
	{
		d.pc++;
		d.update_pc0();
		if (!d.pc)
			d.sti |= D::S_IDLE;
		r = kNext;
	}
	if (d.rptc_next)
	{
		d.rptc = d.rptc_next;
		d.rptc_next = 0;
	}
	if (!(d.icount > 0 && !(d.sti & (D::S_IDLE | D::IN_PLOAD))))
		return kStop;
	return r;
}

// The external-memory transfer step of execute_run's per-instruction prologue.
[[maybe_unused]] KP_NOINLINE KP_COLD_CC void xm_step(D &d)
{
	if (d.sti & D::S_READ)
		d.xm_step_read();
	else
		d.xm_step_write();
}

// xm_step() for the compiled code: xm_step_read / xm_step_write statement for statement, with the
// transfer's required fetches and cycles (functions of st0, which nothing in a frame writes) taken
// from f, set per frame by dsp_fast_run, instead of recomputed at every step. dsp_fast_xm_selftest()
// checks it against the device's own functions on every st0 mode.
[[maybe_unused]] KP_NOINLINE KP_COLD_CC void xm_step_fast(D &d, const DspFast &f)
{
	d.xm_cycles++;
	if (d.xm_cycles < f.xm_first)
		return;
	const int required_fetches = f.xm_req_fetches;
	const int required_cycles = f.xm_req_cycles;
	const u32 st0 = d.st0;
	u32 adr = d.xm_adr;
	int done;
	if (d.sti & D::S_READ)
	{
		u8 v = d.data->read_byte(adr);
		if (st0 & D::ST0_WORD)
		{
			if (st0 & D::ST0_SEL)
			{
				int off = 16 - ((adr & 3) << 3);
				d.txrd = (d.txrd & ~(0xff << off)) | (v << off);
				done = ++d.xm_fetches >= required_fetches;
			}
			else
			{
				int off = 20 - ((adr & 7) << 2);
				d.txrd = (d.txrd & ~(0xf << off)) | ((v & 0xf) << off);
				done = ++d.xm_fetches >= required_fetches;
			}
		}
		else
		{
			if (st0 & D::ST0_SEL)
			{
				int off = 16 - ((adr & 1) << 3);
				d.txrd = (d.txrd & ~(0xff << off)) | (v << off);
				done = ++d.xm_fetches >= required_fetches;
				if (done && d.xm_cycles >= required_cycles)
					d.txrd &= 0xffff00;
			}
			else
			{
				int off = 20 - ((adr & 3) << 2);
				d.txrd = (d.txrd & ~(0xf << off)) | ((v & 0xf) << off);
				done = ++d.xm_fetches >= required_fetches;
				if (done && d.xm_cycles >= required_cycles)
					d.txrd &= 0xffff00;
			}
		}
		if (done && d.xm_cycles >= required_cycles)
		{
			d.xrd = d.txrd;
			d.sti &= ~D::S_READ;
			d.xm_adr = 0;
			d.xm_cycles = 0;
			d.xm_fetches = 0;
		}
		else
			d.xm_adr = adr + 1;
	}
	else
	{
		u8 v;
		if (st0 & D::ST0_WORD)
		{
			if (st0 & D::ST0_SEL)
			{
				int off = 16 - ((adr & 3) << 3);
				v = d.xwr >> off;
				done = ++d.xm_fetches >= required_fetches;
			}
			else
			{
				int off = 20 - ((adr & 7) << 2);
				v = (d.xwr >> off) & 0xf;
				done = ++d.xm_fetches >= required_fetches;
			}
		}
		else
		{
			if (st0 & D::ST0_SEL)
			{
				int off = 16 - ((adr & 1) << 3);
				v = d.xwr >> off;
				done = ++d.xm_fetches >= required_fetches;
			}
			else
			{
				int off = 20 - ((adr & 3) << 2);
				v = (d.xwr >> off) & 0xf;
				done = ++d.xm_fetches >= required_fetches;
			}
		}
		d.data->write_byte(adr, v);
		if (done && d.xm_cycles >= required_cycles)
		{
			d.sti &= ~D::S_WRITE;
			d.xm_adr = 0;
			d.xm_cycles = 0;
			d.xm_fetches = 0;
		}
		else
			d.xm_adr = adr + 1;
	}
}

[[maybe_unused]] KP_NOINLINE KP_COLD_CC void pc0_changed(D &d)
{
	d.update_pc0();
}

// Continue the frame in the interpreter from chain head `ipc` (-1 = decode at the current pc),
// exactly where execute_run() would be.
[[maybe_unused]] KP_NOINLINE void deopt(D &d, DspFast &f, int ipc)
{
	f.deopts++;
	d.jit_run_rest(ipc);
}

// current_cmem_view() on a given st1 (the generated code keeps st1 in a local).
inline u32 crm_view(u32 st1, u32 cvar)
{
	const int crm = (st1 & D::ST1_CRM) >> D::ST1_CRM_SHIFT;
	if (crm == 1)
		return cvar & 0xffff0000;
	else if (crm == 2)
		return cvar << 16;
	return cvar;
}

// crm_view() with the coefficient-RAM mode known at compile time (crm >= 0), else from st1.
inline u32 crm_apply(int crm, u32 st1, u32 cvar)
{
	if (crm < 0)
		return crm_view(st1, cvar);
	if (crm == 1)
		return cvar & 0xffff0000;
	if (crm == 2)
		return cvar << 16;
	return cvar;
}

// get_cmem() with a coefficient update queued, for the generated code: the device sees the local
// sti/st1; the caller takes sti back from dv.sti (get_cmem changes no other promoted state). sti
// goes in by value and comes back through the device, never by reference: a reference to the
// local would keep it in memory for the whole frame function (its address escapes into this
// out-of-line call), and sti is read and written by most steps.
[[maybe_unused]] KP_NOINLINE KP_COLD_CC u32 cmem_slow(D &dv, u8 addr, decltype(D::sti) sti, decltype(D::st1) st1)
{
	dv.sti = sti;
	dv.st1 = st1;
	return dv.get_cmem(addr);
}

// Device chain head for decode key (pc, key), -1 if not decoded.
int lookup_ipc(D &d, std::uint8_t pc, std::uint32_t key)
{
	s16 pnode;
	return d.get_hash(pc, key, &pnode);
}

} // namespace

} // namespace kp

// Pieces of the generated code (see engine/tools/dsp_aot_gen.cpp). Layout of a frame function:
//   head:     locals <- device state, entry check, goto lookup
//   S<k>:     KP_PRE, KP_OP..., terminator (ca/id increments), KP_POST, continuation
//   lookup:   decode-cache lookup at (pc, st1) -> step index s, or hand-off to the interpreter
//   dispatch: switch (s) -> label S<s>
//   slow:     slow_post() for step rk, then repeat / lookup / stop / chained successor
//   done:     device state <- locals; out: execute_run's epilogue
// A step is only ever entered when it is valid, and a valid step's chained successor is valid
// (validate() closes validity over successors), so the continuation needs no check.
//
// The hot DSP state lives in locals named like the members (macc, macc_read, macc_write, aacc,
// ca, id, st1, sti; ba0 read-only; the instruction budget in `ic`), so the compiler keeps it
// in registers. Ops are the register-mode bodies of tools/tms_regops.py (KPR_N), which write the
// state a helper reads back to the device before the call and reload what it writes. Every exit to
// code that sees the device (slow_post, xm_step, the interpreter, the frame end) syncs first.
#define KP_LOCALS() \
	decltype(D::macc) macc = dv.macc, macc_read = dv.macc_read, macc_write = dv.macc_write; \
	decltype(D::aacc) aacc = dv.aacc; \
	decltype(D::ca) ca = dv.ca; \
	decltype(D::id) id = dv.id; \
	decltype(D::st1) st1 = dv.st1; \
	decltype(D::sti) sti = dv.sti; \
	const decltype(D::ba0) ba0 = dv.ba0; \
	int pc0l = dv.m_pc0_line; \
	u32 kp_cv = 0; /* KP_CMEM's slow-path value */ \
	(void)ba0; (void)kp_cv;
#define KP_FLUSH_ALL() \
	dv.macc = macc; dv.macc_read = macc_read; dv.macc_write = macc_write; dv.aacc = aacc; \
	dv.ca = ca; dv.id = id; dv.st1 = st1; dv.sti = sti;
#define KP_RELOAD_ALL() \
	macc = dv.macc; macc_read = dv.macc_read; macc_write = dv.macc_write; aacc = dv.aacc; \
	ca = dv.ca; id = dv.id; st1 = dv.st1; sti = dv.sti; pc0l = dv.m_pc0_line;
// get_cmem() on the locals: with no update queued it is exactly `sti &= ~S_UPDATE; current view`.
// The view uses the frame function's kp_crm (the program's fixed coefficient-RAM mode, -1 = take
// it from st1; see dsp_aot_gen).
#define KP_CMEM(a) \
	(KP_LIKELY(dv.update_counter_count == 0) ? (sti &= ~D::S_UPDATE, crm_apply(kp_crm, st1, dv.cmem[(a)])) \
	                                         : (kp_cv = cmem_slow(dv, (a), sti, st1), sti = dv.sti, kp_cv))
// mpy_d_operand / lmhd_d_operand return their first argument unless a debug forward knob is set;
// dsp_fast_run() never runs compiled code with either knob set.
#define KP_MPY_D(cur, param) u32(cur)
#define KP_LMHD_D(cur, param) u32(cur)
// m_debug_smhd_raw_path in the op bodies (tools/tms_regops.py DEBUG_KNOBS): false; dsp_fast_run()
// never runs compiled code with it set.
#define KP_SMHD_RAW false
// Per step: pre (external memory transfer step, MAC pipeline shift) ... A transfer's first cycles
// only wait (xm_step_read / xm_step_write: xm_cycles++, then return while it is below the first
// fetch cycle, a function of st0 that dsp_fast_run sets per frame in f.xm_first; nothing in a
// frame writes st0), so those run inline and only the fetch cycles call xm_step.
#define KP_PRE() \
	if (KP_UNLIKELY(sti & (D::S_READ | D::S_WRITE))) \
	{ \
		if (int(dv.xm_cycles) + 1 < f.xm_first) \
			dv.xm_cycles++; \
		else \
		{ \
			dv.sti = sti; \
			xm_step_fast(dv, f); \
			sti = dv.sti; \
		} \
	} \
	macc_read = macc_write; \
	macc_write = macc;
// ... ops ...
#define KP_OPFN(n)   // (the generator lists the ops a build uses; nothing to emit per op)
#if defined(KP_AOT_MEMOPS)   // diagnostics: the interpreter's own op functions on device memory
#define KP_OP(n, p) { KP_FLUSH_ALL() static constexpr D::icd i_ = { n, -1, p }; dv.ex_##n(&i_); KP_RELOAD_ALL() }
#else
#define KP_OP(n, p) KPR_##n(p);
#endif
// ... and the epilogue: inline when nothing is pending (the steady-state case), else slow_post()
// through the shared `slow` block. The continuation (goto S<succ> / goto lookup / fall through to
// the next step) follows. update_pc0() only acts when the pc0 line changes; pcnext is a constant,
// so the line is tested inline (against the local copy pc0l: only update_pc0 changes the line, and
// every call of it from here, direct or through slow_post, reloads the copy).
//
// dv.pc: the ops never read it (write_dmem ignores its pc argument; only the branches write it),
// nor do the helpers the steps call, so it is stored only where something reads it: a step with
// the repeat / branch test sets it on entry (KP_PC; slow_post and the branches work on it) and
// keeps it current, a KP_POSTF step stores it only on its way out (frame end, pc0 line change,
// lookup). Everything that leaves the compiled code (done, lookup, slow, the hand-offs) sees the
// device's pc as the interpreter would have it.
#define KP_PC(p) dv.pc = (p);
// The instruction budget: the board gives a frame 1 << 16 instructions and a frame takes a few
// hundred, so the budget only runs out in a program that loops (back branches). A KP_POSTF step
// counts it but does not test it: the tests sit where execution enters compiled steps (dispatch)
// and in KP_POST steps (every loop passes through a branch step), and they leave the compiled code
// for the interpreter once the budget is down to kBudgetMargin, more than the longest run of
// KP_POSTF steps between two tests can use (a run goes up in pc and ends at the wrap to pc 0, so
// it is at most 256 steps). The interpreter then ends the frame exactly where it would have.
constexpr int kBudgetMargin = 256;
#define KP_POST(k, pcnext, wrap) \
	if (KP_UNLIKELY((int(dv.m_pending_pre_transfer != D::pending_pre_transfer_type::none) | int(dv.rptc) | int(dv.rptc_next) | int(sti & D::S_BRANCH)) != 0)) \
	{ \
		rk = k; \
		goto slow; \
	} \
	ic--; \
	dv.pc = pcnext; \
	if (KP_UNLIKELY(pc0l != ((pcnext) == 0 ? 0 : 1))) \
	{ \
		pc0_changed(dv); \
		pc0l = dv.m_pc0_line; \
	} \
	wrap \
	if (KP_UNLIKELY(ic <= kBudgetMargin || (sti & (D::S_IDLE | D::IN_PLOAD)))) \
	{ \
		if (ic <= 0 || (sti & (D::S_IDLE | D::IN_PLOAD))) \
			goto done; \
		goto lookup; /* budget low: dispatch hands the rest of the frame to the interpreter */ \
	}
// The epilogue of a step that never runs with the repeat / branch state set (the generator decides:
// no rptk or branch in the step, not at a pc a pending repeat can reach). The state is clear on
// entry (frame head) and only rptk / branch steps set it, so KP_POST's test is known false here.
// pc0test = 0 drops the pc0 line test where it is known false: a step entered only from its chained
// predecessor, with pcnext >= 2. Along a chain the line is 1 from the first step after pc 0 on
// (every step that moves to pc 0 or 1, and every entry step, where a frame or a branch can start
// with the line still 0, keeps the test), so such a step finds it 1 and leaves it 1.
// endtest = 0 drops the frame end test where it is known false: no budget test (kBudgetMargin), and
// S_IDLE is set only by the idle op and the wrap to pc 0, IN_PLOAD only by a host write (between
// frames), so only a step with the idle op or the wrap can end the frame.
#define KP_POSTF(k, pcnext, wrap, pc0test, endtest) \
	ic--; \
	if ((pc0test) && KP_UNLIKELY(pc0l != ((pcnext) == 0 ? 0 : 1))) \
	{ \
		dv.pc = pcnext; \
		pc0_changed(dv); \
		pc0l = dv.m_pc0_line; \
	} \
	wrap \
	if ((endtest) && KP_UNLIKELY(sti & (D::S_IDLE | D::IN_PLOAD))) \
	{ \
		dv.pc = pcnext; \
		goto done; \
	}
// Function head: locals, entry check, then the lookup (placed after the steps, so the steps lay out
// in program order). A frame entered with the repeat / branch / pre-transfer state set (a frame
// cut off inside a repeat, a restored snapshot) runs in the interpreter: KP_POSTF assumes it clear.
#define KP_HEAD() \
	int s = 0, rk = 0, ic = dv.icount; \
	KP_LOCALS() \
	if (!(ic > 0 && !(sti & (D::S_IDLE | D::IN_PLOAD)))) \
		goto done; \
	if (KP_UNLIKELY((int(dv.m_pending_pre_transfer != D::pending_pre_transfer_type::none) | int(dv.rptc) | int(dv.rptc_next) | int(sti & D::S_BRANCH)) != 0 \
	                || (kp_crm >= 0 && int((st1 & D::ST1_CRM) >> D::ST1_CRM_SHIFT) != kp_crm))) \
	{ \
		deopt(dv, f, -1); \
		goto out; \
	} \
	goto lookup;
// After the steps: lookup, dispatch table start (the generator emits the cases), ...
#define KP_DISPATCH() \
lookup: \
	{ \
		const int ipc = lookup_ipc(dv, dv.pc, st1); \
		if (ipc < 0 || f.step_of_ipc[ipc] < 0) \
		{ \
			KP_FLUSH_ALL() \
			dv.icount = ic; \
			deopt(dv, f, ipc); \
			goto out; \
		} \
		s = f.step_of_ipc[ipc]; \
	} \
dispatch: \
	if (KP_UNLIKELY(ic <= kBudgetMargin)) /* see kBudgetMargin */ \
	{ \
		KP_FLUSH_ALL() \
		dv.icount = ic; \
		deopt(dv, f, f.ipc_of_step[s]); \
		goto out; \
	} \
	switch (s) \
	{
// ... close the switch, then the shared slow epilogue.
#define KP_SLOW(steps) \
	default: /* not an entry point: the interpreter continues at this step's chain head */ \
		KP_FLUSH_ALL() \
		dv.icount = ic; \
		deopt(dv, f, f.ipc_of_step[s]); \
		goto out; \
	} \
slow: \
	f.slow_posts++; \
	KP_FLUSH_ALL() \
	dv.icount = ic; \
	s = slow_post(dv); \
	ic = dv.icount; \
	KP_RELOAD_ALL() \
	switch (s) \
	{ \
	case kRepeat: s = rk; goto dispatch; \
	case kStop: goto done; \
	case kLookup: goto lookup; \
	default: s = steps[rk].succ; if (s < 0) goto lookup; goto dispatch; \
	}
#define KP_TAIL() \
done: \
	KP_FLUSH_ALL() \
	dv.icount = ic; \
out: \
	if (dv.icount > 0) \
		dv.icount = 0;
// Optional attributes for the generated frame functions (none by default).
#ifndef KP_AOT_FN
#define KP_AOT_FN
#endif

namespace kp {
namespace {
#if __has_include("dsp_aot_gen.inc") && !defined(KP_DSP_AOT_NONE)
#include "cpu/tms57002/tms57002_regops.hxx"   // tools/tms_regops.py, next to tms57002.hxx
#include "dsp_aot_gen.inc"
#else
const AotProgram *const kAotPrograms = nullptr;
constexpr int kAotProgramCount = 0;
#endif

const AotProgram *find_program(std::uint64_t hash)
{
	for (int i = 0; i < kAotProgramCount; i++)
		if (kAotPrograms[i].hash == hash)
			return &kAotPrograms[i];
	return nullptr;
}

// Compare every generated step with the device's decode cache (after any cache change).
void validate(D &d, DspFast &f)
{
	const AotProgram &p = *f.prog;
	std::fill(std::begin(f.step_of_ipc), std::end(f.step_of_ipc), std::int16_t(-1));
	f.valid_steps = 0;
	f.total_steps = p.step_count;
	for (int s = 0; s < p.step_count; s++)
	{
		const AotStep &st = p.steps[s];
		const int head = lookup_ipc(d, st.pc, st.key);
		f.ipc_of_step[s] = std::int16_t(head);
		bool ok = head >= 0;
		int cur = head;
		int term_next = -1;
		for (int j = 0; ok && j < st.op_count; j++)
		{
			const AotOp &e = p.ops[st.op_first + j];
			if (cur < 0 || cur >= D::IBS)
			{
				ok = false;
				break;
			}
			const D::icd &i = d.cache.inst[cur];
			const bool last = j == st.op_count - 1;
			if (i.op != e.op || (!last && i.param != e.param) || (last != (i.op < 4)))
			{
				ok = false;
				break;
			}
			if (last)
				term_next = i.next;
			else
				cur = i.next;
		}
		if (ok)
		{
			if (st.succ < 0)
				ok = term_next == -1;
			else
				ok = term_next >= 0 && term_next == lookup_ipc(d, p.steps[st.succ].pc, p.steps[st.succ].key);
		}
		f.valid[s] = ok;
	}
	// A step is usable only if its chained successor is (the generated code continues into it
	// without a check): close validity backwards along the chains. Steps are sorted by pc and a
	// successor is at pc + 1, so a descending pass settles every chain except across the pc 255 -> 0
	// wrap, which a further pass covers; repeat until stable to stay correct for any order.
	for (bool changed = true; changed;)
	{
		changed = false;
		for (int s = p.step_count - 1; s >= 0; s--)
			if (f.valid[s] && p.steps[s].succ >= 0 && !f.valid[p.steps[s].succ])
			{
				f.valid[s] = 0;
				changed = true;
			}
	}
	for (int s = 0; s < p.step_count; s++)
		if (f.valid[s])
		{
			f.step_of_ipc[f.ipc_of_step[s]] = std::int16_t(s);
			f.valid_steps++;
		}
	f.hused = d.cache.hused;
	f.iused = d.cache.iused;
}

} // namespace

void dsp_fast_prefault()
{
#if defined(__linux__)
	// The compiled programs are megabytes of code, each first run when its DSP program is loaded
	// (a program change, in the audio thread). Touch every page of this module's executable
	// mapping now so that first run does not take page faults (reads from storage on a device).
	const auto self = reinterpret_cast<std::uintptr_t>(&dsp_fast_prefault);
	if (FILE *m = std::fopen("/proc/self/maps", "r"))
	{
		char line[512];
		while (std::fgets(line, sizeof line, m))
		{
			unsigned long lo = 0, hi = 0;
			char perms[8] = {};
			if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[2] != 'x' || self < lo || self >= hi)
				continue;
			volatile unsigned char sink = 0;
			for (std::uintptr_t a = lo; a < hi; a += 4096)
				sink = sink + *reinterpret_cast<const volatile unsigned char *>(a);
			(void)sink;
		}
		std::fclose(m);
	}
#endif
}

void dsp_fast_enable(bool on) { g_enabled = on; }

int dsp_fast_xm_selftest(D &d, int cases, unsigned seed)
{
	// Run the device's xm_step_read / xm_step_write and xm_step_fast from the same states, step by
	// step through whole transfers and from random mid-transfer states, on every st0 transfer mode.
	struct S
	{
		u32 st0, sti, xm_adr, txrd, xrd, xwr;
		u8 xm_cycles, xm_fetches, mem;
		bool operator==(const S &o) const
		{
			return st0 == o.st0 && sti == o.sti && xm_adr == o.xm_adr && txrd == o.txrd && xrd == o.xrd && xwr == o.xwr &&
			       xm_cycles == o.xm_cycles && xm_fetches == o.xm_fetches && mem == o.mem;
		}
	};
	auto &mem = d.data->bytes();
	const S saved = { d.st0, d.sti, d.xm_adr, d.txrd, d.xrd, d.xwr, d.xm_cycles, d.xm_fetches, 0 };
	u32 rng = seed * 2654435761u + 1;
	auto rnd = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
	auto load = [&](const S &x, u32 adr) {
		d.st0 = x.st0; d.sti = x.sti; d.xm_adr = x.xm_adr; d.txrd = x.txrd; d.xrd = x.xrd; d.xwr = x.xwr;
		d.xm_cycles = x.xm_cycles; d.xm_fetches = x.xm_fetches;
		if (adr < mem.size()) mem[adr] = x.mem;
	};
	auto take = [&](u32 adr) {
		return S{ d.st0, d.sti, d.xm_adr, d.txrd, d.xrd, d.xwr, d.xm_cycles, d.xm_fetches, u8(adr < mem.size() ? mem[adr] : 0) };
	};
	int bad = 0;
	for (int c = 0; c < cases; c++)
	{
		const u32 mode = ((c & 1) ? D::ST0_WORD : 0) | ((c & 2) ? D::ST0_SEL : 0) | ((c & 4) ? D::ST0_SRAM : 0);
		const bool read = (c & 8) != 0;
		const bool mid = (c & 16) != 0;
		S x;
		x.st0 = (saved.st0 & ~u32(D::ST0_WORD | D::ST0_SEL | D::ST0_SRAM)) | mode;
		x.sti = (saved.sti & ~u32(D::S_READ | D::S_WRITE)) | (read ? D::S_READ : D::S_WRITE);
		const u32 span = mem.empty() ? 64u : u32(mem.size());
		x.xm_adr = mid ? rnd() % span : (rnd() % span) & ~7u;
		x.txrd = rnd() & (mid ? 0xffffffffu : 0x00ffffffu);
		x.xrd = rnd();
		x.xwr = rnd() & ((c & 32) ? 0xffffffffu : 0x00ffffffu);
		x.xm_cycles = u8(mid ? rnd() % 20 : 0);
		x.xm_fetches = u8(mid ? rnd() % 8 : 0);
		x.mem = 0;
		DspFast f;
		for (int step = 0; step < 24; step++)
		{
			const u32 adr = x.xm_adr;
			x.mem = u8(rnd());
			load(x, adr);
			if (d.sti & D::S_READ)
				d.xm_step_read();
			else
				d.xm_step_write();
			const S want = take(adr);
			load(x, adr);
			f.xm_req_cycles = d.xm_required_cycles();
			f.xm_req_fetches = d.xm_required_fetches();
			f.xm_first = std::max(1, f.xm_req_cycles - f.xm_req_fetches + 1);
			xm_step_fast(d, f);
			const S got = take(adr);
			if (!(got == want))
			{
				bad++;
				break;
			}
			x = got;
			if (!(x.sti & (D::S_READ | D::S_WRITE)))
				break;
		}
	}
	load(saved, ~0u);
	return bad;
}
void dsp_fast_budget_cap(int n) { g_budget_cap = n; }
void dsp_fast_stream_raw(bool on) { g_stream_raw = on; }

bool dsp_stream_update_raw(D &d, const std::array<u32, 4> &in, std::array<u32, 4> &out)
{
	// sound_stream_update() statement for statement, on the board's 24-bit words instead of a float
	// stream. The board's float round trip is exact both ways: an input is
	// s32(float(sext24(w)) / scale * scale) & 0xffffff = w (24-bit values, scale a power of two), an
	// output u32(s32(double(float(s32(v << 8) / 2^31)) * 2^31) >> 8) & 0xffffff = v & 0xffffff.
	// The test input sine (m_input_sine) is not handled here: false, the caller takes the stream path.
	if (!g_stream_raw || d.m_input_sine)
		return false;
	d.maybe_state_zap();
	d.m_sound_updates++;
	if (d.serial_cycle_model_enabled())
		d.finalize_serial_output_build();
	std::array<u32, 4> frame{};
	for (u8 i = 0; i < 4; i++)
	{
		const u32 stream_value = in[i] & 0x00ffffffU;
		switch (d.m_serial_input_timing_mode)
		{
		case D::serial_input_timing_mode::sample_delay_1:
			frame[i] = BIT(d.m_serial_input_active_valid, i) ? d.m_serial_input_active[i] : stream_value;
			break;

		case D::serial_input_timing_mode::same_sample:
		default:
			frame[i] = BIT(d.m_serial_input_valid, i) ? d.m_serial_input_latch[i] : stream_value;
			break;
		}
	}
	if (d.serial_cycle_model_enabled())
	{
		d.m_serial_input_frame = frame;
		d.start_serial_frame(frame);
	}
	else
	{
		d.prepare_serial_inputs(frame);
	}
	d.m_serial_input_active = d.m_serial_input_latch;
	d.m_serial_input_active_valid = d.m_serial_input_valid;
	d.m_serial_input_valid = 0;
	for (int i = 0; i < 4; i++)
		out[size_t(i)] = (d.m_stream_output_raw ? d.so[i] : d.serial_output_pin(i)) & 0x00ffffffU;
	if (d.m_stream_sync_enabled)
		d.sync_w(1);
	return true;
}
bool dsp_fast_enabled() { return g_enabled; }
int dsp_fast_program_count() { return kAotProgramCount; }

void dsp_fast_run(D &d, DspFast &f)
{
	if (KP_UNLIKELY(g_budget_cap > 0) && d.icount > g_budget_cap)
		d.icount = g_budget_cap;
	// The generated code reproduces execute_run() for the board's configuration: no serial cycle
	// model, no runtime dynarec. Anything else, or no generated code for this program: interpreter.
	if (!g_enabled || kAotProgramCount == 0 || d.serial_cycle_model_enabled() || d.m_dynarec_default
		|| d.m_debug_mpy_smhd_forward || d.m_debug_lmhd_srbd_forward || d.m_debug_smhd_raw_path)
	{
		f.interp_frames++;
		d.execute_run();
		return;
	}
	if (d.m_program_version != f.version)
	{
		f.version = d.m_program_version;
		f.prog = find_program(d.jit_program_hash());
		f.hused = f.iused = -1;
	}
	if (!f.prog)
	{
		f.interp_frames++;
		d.execute_run();
		return;
	}
	if (d.cache.hused != f.hused || d.cache.iused != f.iused)
		validate(d, f);
	f.fast_frames++;
	f.xm_req_cycles = d.xm_required_cycles();   // as xm_step_read computes them at every step
	f.xm_req_fetches = d.xm_required_fetches();
	f.xm_first = std::max(1, f.xm_req_cycles - f.xm_req_fetches + 1);
	f.prog->run(d, f);
}

} // namespace kp
