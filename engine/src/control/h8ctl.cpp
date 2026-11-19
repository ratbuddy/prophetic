#include "control/h8ctl.h"

#include <cstdio>
#include <cstring>

namespace kp::ctl {

using namespace kp::h8;

namespace {
constexpr u32 kMagicReturn = 0x00fffe;   // never executed: marks the outermost frame
constexpr u32 kIsrStack = 0x040600;      // unused task-5 stack slot, used for injected ISRs
constexpr u32 kSsr = 0xfffb4, kRdr = 0xfffb5, kPcdr = 0xfffd7, kPbdr = 0xfffd6;
// The program-change routine's fixed wait (v2.0: mov.l #0x38000, er0; dec.l #1, er0; bne at
// 0x0124BC; 6 states per iteration, 229,376 iterations = 5,376 frames): it lets the clear program
// that was just loaded into DSP1 and DSP3 wipe their delay memory before the new programs arrive
// (oracle: 5,454 / 5,581 frames from the clear load to the next load, the rest being H8 work).
constexpr u32 kDelayLoopPc = 0x0124bc;
constexpr int kDelayLoopStates = 6;
constexpr int kStatesPerFrame = 256;   // 12.288 MHz / 48 kHz
}

H8Control::H8Control(ProphecyBoard &board) : board_(board)
{
	m_.hooks = this;
	c_.m = &m_;
	c_.hooks = this;
	c_.delay_pc = kDelayLoopPc;
	adc_.fill(0x8000);   // 10-bit 0x200 (centre), left-aligned: the oracle's power-on default
	adc_[7] = 0xffc0;    // AN7 = backup-battery monitor: healthy (full scale), as in the oracle
}

bool H8Control::load(const RomView &rom, const std::uint8_t *ram, std::size_t size)
{
	if (size < m_.sram.size() + m_.onchip.size())
		return false;
	m_.rom = &rom;
	std::memcpy(m_.sram.data(), ram, m_.sram.size());
	std::memcpy(m_.onchip.data(), ram + m_.sram.size(), m_.onchip.size());
	read_tasks(rom);
	return true;
}

void H8Control::read_tasks(const RomView &rom)
{
	for (int n = 0; n < 5; n++)   // RTOS task descriptors: ROM 0xF4 + 10n = {entry, SP, priority}
	{
		const u32 a = 0xf4 + 10 * n;
		tasks_[n].entry = rom.r32(a) & 0xffffff;
		tasks_[n].stack = rom.r32(a + 4) & 0xffffff;
		tasks_[n].prio = rom.r16(a + 8);
		tasks_[n].pending = false;
		tasks_[n].budget = 0;
	}
}

void H8Control::boot(const RomView &rom)
{
	m_.rom = &rom;
	std::fill(m_.sram.begin(), m_.sram.end(), u8(0));
	std::fill(m_.onchip.begin(), m_.onchip.end(), u8(0));
	std::fill(m_.io.begin(), m_.io.end(), u8(0));
	read_tasks(rom);
	for (auto &q : pending_)
		q.clear();
	rx_pending_ = -1;
	running_ = -1;
	in_isr_ = 0;
	// The reset handler (vector 0) sets up the hardware and the RTOS, runs the boot routines (UART,
	// DSP program loads, ...), starts the first task and enters the RTOS dispatcher; the dispatcher's
	// switch into a task is a return here (the native scheduler below runs the tasks instead).
	busy_states_ = 0;
	c_ = Cpu{};
	c_.m = &m_;
	c_.hooks = this;
	c_.delay_pc = kDelayLoopPc;
	c_.er[7] = kIsrStack;   // the call's return address goes there; the handler sets its own SP
	kp::h8::call(c_, rom.r32(0) & 0xffffff, kMagicReturn);
	const u64 calls = c_.calls;
	c_ = Cpu{};
	c_.m = &m_;
	c_.hooks = this;
	c_.delay_pc = kDelayLoopPc;
	c_.calls = calls;
	run_scheduler();
}

std::vector<u8> H8Control::ram_image() const
{
	std::vector<u8> r(m_.sram);
	r.insert(r.end(), m_.onchip.begin(), m_.onchip.end());
	return r;
}

void H8Control::delay_iteration() { busy_states_ += kDelayLoopStates; }

std::uint64_t H8Control::due() const { return board_.frames() + busy_states_ / kStatesPerFrame; }

void H8Control::unsupported(u32 pc, const char *what)
{
	if (unsupported_count++ < 20)
		std::fprintf(stderr, "h8ctl: unsupported at %06x: %s\n", pc, what);
}

// ---- hardware -------------------------------------------------------------------------------
u8 H8Control::io_read8(u32 a, u8 cur)
{
	if (a == kSsr)
		return rx_pending_ >= 0 ? u8((cur & 0x07) | 0xc0) : u8((cur & 0x07) | 0x80);   // TDRE, RDRF when a byte is ready
	if (a == kRdr)
		return rx_pending_ >= 0 ? u8(rx_pending_) : cur;
	if (a == kPbdr)
		return 0;   // DSP update queues never reported full: pump_dsp() paces writes instead
	if (a >= 0xfffe0 && a < 0xfffe8)   // ADDRA..ADDRD (high, low); group 1 (AN4-7) when CH2 set
	{
		const int ch = ((m_.io[0xfffe8 - 0xfff10] & 0x04) ? 4 : 0) + int(a - 0xfffe0) / 2;
		const u16 v = adc_[ch];
		return (a & 1) ? u8(v & 0xc0) : u8(v >> 8);
	}
	if (a == 0xfffe8)   // ADCSR: conversion always complete (ADF)
		return u8(cur | 0x80);
	return cur;
}

void H8Control::io_write8(u32 a, u8 v)
{
	if (a == kPcdr)
		for (int d = 0; d < 3; d++)
			pending_[d].push_back({ due(), kMute, 0, u32((v >> 7) & 1), {} });
	if (a == kSsr && !(v & 0x40))
		rx_pending_ = -1;   // ISR cleared RDRF: byte consumed
}

// ---- DSP host port --------------------------------------------------------------------------
void H8Control::send_dma(int index, u32 buf)
{
	if (index < 1 || index > 3)
	{
		unsupported(0x01a2f4, "dsp_send_dma index");
		return;
	}
	const int d = index - 1;
	// Coefficient writes still queued for this DSP belong to the program being replaced; on the
	// hardware they reach the DSP before the PLOAD and its CMEM init then overwrites them.
	auto &q = pending_[d];
	for (auto it = q.begin(); it != q.end();)
		if (it->kind == kWrite)
		{
			it = q.erase(it);
			dropped_writes++;
		}
		else
			++it;
	auto u16at = [&](u32 a) { return u32((m_.rd8(a) << 8) | m_.rd8(a + 1)); };
	const u32 n1 = u16at(buf);
	const u32 n2 = u16at(buf + 2 + n1);
	Op o{ due(), kLoad, 0, n1, {} };
	o.bytes.reserve(n1 + n2);
	for (u32 i = 0; i < n1; i++)
		o.bytes.push_back(m_.rd8(buf + 2 + i));
	for (u32 i = 0; i < n2; i++)
		o.bytes.push_back(m_.rd8(buf + 4 + n1 + i));
	q.push_back(std::move(o));
}

void H8Control::apply_load(int d, const Op &o)
{
	board_.pload(d, 0);
	for (u32 i = 0; i < o.value; i++)
		board_.data_w(d, o.bytes[i]);
	board_.cload(d, 0);
	for (std::size_t i = o.value; i < o.bytes.size(); i++)
		board_.data_w(d, o.bytes[i]);
	board_.cload(d, 1);
	board_.pload(d, 1);
}

void H8Control::pump_dsp()
{
	const u64 now = board_.frames();
	for (int d = 0; d < 3; d++)
	{
		auto &q = pending_[d];
		while (!q.empty() && q.front().due <= now)
		{
			const Op &o = q.front();
			if (o.kind == kWrite)
			{
				if (board_.update_queue(d) >= 16)
					break;   // the H8 waits on PBDR for room in the DSP's update registers
				board_.cload(d, 0);
				board_.data_w(d, o.sa);
				board_.data_w(d, u8(o.value >> 24));
				board_.data_w(d, u8(o.value >> 16));
				board_.data_w(d, u8(o.value >> 8));
				board_.data_w(d, u8(o.value));
				board_.cload(d, 1);
			}
			else if (o.kind == kLoad)
				apply_load(d, o);
			else
				board_.mute(d, int(o.value));
			q.pop_front();
		}
		if (q.size() > 4096)   // runaway guard: never block the audio thread (coefficient writes only)
			for (auto it = q.begin(); it != q.end();)
				if (it->kind == kWrite)
				{
					it = q.erase(it);
					dropped_writes++;
				}
				else
					++it;
	}
}

// ---- kernel replacement ---------------------------------------------------------------------
void H8Control::wake(int n)
{
	tasks_[n].pending = true;
	if (!in_isr_ && running_ >= 0 && tasks_[n].prio < tasks_[running_].prio)
	{
		tasks_[n].pending = false;
		run_task(n);   // preempt the running task, as the RTOS would
	}
}

bool H8Control::call(u32 a, Cpu &c)
{
	switch (a)
	{
	case 0x018252:   // param_dispatch: observe, then run the translated code
		if (on_dispatch)
			on_dispatch(c.r16(1), c.er[0]);
		return false;
	case 0x019e0a:   // dsp_cload_write(er0 value, r1l dsp 1-3, r2l coefficient address)
	{
		const int d = int(c.r8(9)) - 1;
		if (d >= 0 && d < 3)
		{
			pending_[d].push_back({ due(), kWrite, c.r8(10), c.er[0], {} });
			dsp_writes++;
		}
		return true;
	}
	case 0x01a2f4:   // dsp_send_dma(r1l index, er5 buffer)
		send_dma(c.r8(9), c.er[5] & 0xffffff);
		program_sends++;
		return true;
	case 0x0109c6:   // start task r0
		if (c.r16(0) < 5) wake(c.r16(0));
		return true;
	case 0x010aee:   // kill task r0 (restart idiom: kill then start)
		if (c.r16(0) < 5) tasks_[c.r16(0)].pending = false;
		return true;
	case 0x010cd8:   // post mailbox r0: mailbox 1 wakes the message task (task 0)
		if (c.r16(0) == 1) { tasks_[0].budget++; wake(0); }
		return true;
	case 0x010bec:   // task exit
		c.unwind = true;
		return true;
	case 0x010d38:   // wait on own mailbox
		if (running_ >= 0 && tasks_[running_].budget > 0) { tasks_[running_].budget--; return true; }
		c.unwind = true;
		return true;
	case 0x010d9e:   // mailbox init
		return true;
	}
	return false;
}

void H8Control::run_task(int n)
{
	const Cpu saved = c_;
	const int prev = running_;
	running_ = n;
	c_.er[7] = tasks_[n].stack;
	c_.unwind = false;
	c_.spins = 0;
	kp::h8::call(c_, tasks_[n].entry, kMagicReturn);
	const u64 calls = c_.calls;
	c_ = saved;
	c_.calls = calls;
	running_ = prev;
}

void H8Control::run_scheduler()
{
	for (int guard = 0; guard < 64; guard++)
	{
		int best = -1;
		for (int n = 0; n < 5; n++)
			if (tasks_[n].pending && (best < 0 || tasks_[n].prio < tasks_[best].prio))
				best = n;
		if (best < 0)
			return;
		tasks_[best].pending = false;
		run_task(best);
	}
}

void H8Control::isr(u32 vector_entry)
{
	const Cpu saved = c_;
	in_isr_++;
	c_.er[7] = kIsrStack;
	c_.unwind = false;
	c_.spins = 0;
	kp::h8::call(c_, vector_entry, kMagicReturn);
	in_isr_--;
	const u64 calls = c_.calls;
	c_ = saved;
	c_.calls = calls;
	run_scheduler();
}

void H8Control::uart_byte(u8 b)
{
	// The V55 raises IRQ1 to grant the H8 receive credit before it transmits (RAM 0x04829E).
	if (m_.rd8(0x048c22) == 0 && ((m_.rd8(0x04829e) << 8) | m_.rd8(0x04829f)) == 0)
		isr(0x01ad0e);
	busy_states_ = 0;
	rx_pending_ = b;
	isr(0x01aefa);
	rx_pending_ = -1;
}

void H8Control::tick()
{
	busy_states_ = 0;
	isr(0x0106ae);
}

} // namespace kp::ctl
