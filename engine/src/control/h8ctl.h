// Prophecy control layer: the H8 firmware's logic, translated to C++ (generated/h8ctl from the
// user's ROM), driven by a native scheduler that replaces the H8 RTOS and interrupt hardware.
//
//   uart_byte()  - one V55 -> H8 byte: runs the translated SCI receive ISR, then ready tasks
//   tick()       - the 246.15 Hz watchdog-interval interrupt: starts task 3, then ready tasks
//   pump_dsp()   - once per DSP frame: moves queued coefficient writes into the DSP update
//                  registers while they have room (the H8 waits on PBDR for the same thing)
//
// Kernel calls are replaced natively: start task (0x0109C6), post (0x010CD8), wait (0x010D38),
// exit (0x010BEC), mailbox init (0x010D9E). Tasks run to completion with priority preemption.
// DSP host-port routines (0x019E0A CLOAD write, 0x01A2F4 DMA program send) and the mute line
// (port C bit 7) go to the ProphecyBoard. Addresses are v2.0 (korgpro20).
#pragma once

#include "control/h8rt.h"
#include "control/rom_view.h"
#include "prophecy_board.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace kp::ctl {

class H8Control : public kp::h8::Hooks
{
public:
	static constexpr int kFramesPerTick = 195;   // 49,920 H8 cycles / 256 per 48 kHz frame

	explicit H8Control(ProphecyBoard &board);
	// ram = SRAM 0x40000-0x5FFFF (128 KiB) followed by on-chip RAM 0xFFD10-0xFFF0F (512 B).
	bool load(const RomView &rom, const std::uint8_t *ram, std::size_t size);
	// Power-on instead of a snapshot: cleared RAM and I/O, the firmware's reset handler (its
	// hardware and RTOS set-up and the boot routine that loads the DSPs), then the tasks it starts.
	// The V55's power-on messages and the frames that follow are the caller's (Synth::load).
	void boot(const RomView &rom);
	// The RAM in load()'s layout (SRAM, then on-chip RAM): ram.bin.
	std::vector<std::uint8_t> ram_image() const;

	void uart_byte(std::uint8_t b);
	void tick();
	void pump_dsp();

	void set_adc(int ch, std::uint16_t v) { if (ch >= 0 && ch < 8) adc_[ch] = v; }

	std::function<void(std::uint16_t slot, std::uint32_t input)> on_dispatch;   // param_dispatch entries
	std::uint64_t unsupported_count = 0, dsp_writes = 0, program_sends = 0, dropped_writes = 0;
	kp::h8::Cpu &cpu() { return c_; }
	kp::h8::Mem &mem() { return m_; }

private:
	struct Task { std::uint32_t entry = 0, stack = 0; int prio = 7; bool pending = false; int budget = 0; };
	// A host-port operation for one DSP, in issue order: a coefficient write (sa, value), a program
	// load (bytes: the PLOAD data, then from `value` on the CMEM init) or the mute line (value). It is
	// applied from board frame `due` on: the H8's time in its timed busy-wait (delay_iteration) delays
	// what it does after it, as on the hardware.
	enum : std::uint8_t { kWrite, kLoad, kMute };
	struct Op
	{
		std::uint64_t due;
		std::uint8_t kind, sa;
		std::uint32_t value;
		std::vector<std::uint8_t> bytes;
	};

	bool call(std::uint32_t addr, kp::h8::Cpu &c) override;
	std::uint8_t io_read8(std::uint32_t a, std::uint8_t cur) override;
	void io_write8(std::uint32_t a, std::uint8_t v) override;
	void unsupported(std::uint32_t pc, const char *what) override;
	void delay_iteration() override;
	std::uint64_t due() const;
	void apply_load(int d, const Op &o);

	void read_tasks(const RomView &rom);
	void wake(int n);
	void run_task(int n);
	void run_scheduler();
	void isr(std::uint32_t vector_entry);
	void send_dma(int index, std::uint32_t buf);

	ProphecyBoard &board_;
	kp::h8::Mem m_;
	kp::h8::Cpu c_;
	std::array<Task, 5> tasks_{};
	int running_ = -1;
	int in_isr_ = 0;
	int rx_pending_ = -1;
	std::array<std::uint16_t, 8> adc_{};
	std::array<std::deque<Op>, 3> pending_;
	std::uint64_t busy_states_ = 0;   // H8 states in timed busy-waits since the current event began
};

} // namespace kp::ctl
