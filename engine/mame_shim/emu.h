// Minimal stand-in for MAME's emu.h: just enough of the device, memory and sound API for the
// vendored, unmodified TMS57002 core (third_party/tms57002) to compile and run standalone.
// This file is ours; it contains no MAME code.
#pragma once

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

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using offs_t = u32;

template <typename T, typename U> constexpr T BIT(T x, U n) noexcept { return (x >> n) & T(1); }
template <typename T, typename U, typename V> constexpr T BIT(T x, U n, V w) { return (x >> n) & ((T(1) << w) - 1); }

#define ATTR_COLD
#define NAME(x) x, #x

[[noreturn]] inline void fatalerror(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::abort();
}
inline void popmessage(const char *, ...) {}

enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum { AS_PROGRAM = 0, AS_DATA = 1, AS_IO = 2, AS_OPCODES = 3 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };
constexpr int SAMPLE_RATE_ADAPTIVE = -1;
constexpr int STREAM_SYNCHRONOUS = 1;

class machine_config {};
class device_t;
using device_type = int;
#define DECLARE_DEVICE_TYPE(Type, Class) extern const device_type Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) const device_type Type = 0;

class address_map_entry { public: address_map_entry &ram() { return *this; } };
class address_map
{
public:
	address_map_entry &operator()(offs_t, offs_t) { return m_entry; }
private:
	address_map_entry m_entry;
};
struct address_map_constructor
{
	address_map_constructor() = default;
	template <typename F, typename O> address_map_constructor(F, O) {}
};
#define FUNC(x) &x

struct address_space_config
{
	const char *name;
	int data_width, addr_width, addr_shift;
	address_space_config(const char *n, endianness_t, int dw, int aw, int shift = 0, address_map_constructor = {})
		: name(n), data_width(dw), addr_width(aw), addr_shift(shift) {}
};

// Flat RAM for the internal program space (32-bit words, word-addressed) and the external
// data space (bytes). Accesses outside the mapped RAM read 0 and drop writes, like an
// unmapped MAME range.
class address_space
{
public:
	address_space(int data_width, size_t mapped_units) : m_width(data_width)
	{
		if (data_width == 32)
			m_words.assign(mapped_units, 0);
		else
			m_bytes.assign(mapped_units, 0);
	}
	u32 read_dword(offs_t a) const { return a < m_words.size() ? m_words[a] : 0; }
	void write_dword(offs_t a, u32 v) { if (a < m_words.size()) m_words[a] = v; }
	u8 read_byte(offs_t a) const { return a < m_bytes.size() ? m_bytes[a] : 0; }
	void write_byte(offs_t a, u8 v) { if (a < m_bytes.size()) m_bytes[a] = v; }
	std::vector<u32> &words() { return m_words; }
	std::vector<u8> &bytes() { return m_bytes; }
private:
	int m_width;
	std::vector<u32> m_words;
	std::vector<u8> m_bytes;
};

struct device_memory_interface
{
	using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;
};

namespace util { class disasm_interface { public: virtual ~disasm_interface() = default; }; }

class devcb_write_line
{
public:
	explicit devcb_write_line(device_t &) {}
	void operator()(int) {}
	struct binder { template <typename... A> binder &set(A &&...) { return *this; } };
	binder bind() { return {}; }
};

struct state_entry
{
	state_entry &noshow() { return *this; }
	state_entry &mask(u64) { return *this; }
	state_entry &formatstr(const char *) { return *this; }
};

class device_t
{
public:
	device_t(const char *tag, u32 clock) : m_tag(tag ? tag : ""), m_clock(clock) {}
	virtual ~device_t() = default;
	const char *tag() const { return m_tag.c_str(); }
	u32 clock() const { return m_clock; }
	template <typename... A> void logerror(const char *fmt, A... args) const { std::fprintf(stderr, fmt, args...); }
	template <typename T> void save_item(T &, const char *, int = 0) {}
	virtual void device_start() {}
	virtual void device_reset() {}
	virtual void device_post_load() {}
	virtual void device_stop() {}
private:
	std::string m_tag;
	u32 m_clock;
};

class cpu_device : public device_t, public device_memory_interface
{
public:
	cpu_device(const machine_config &, device_type, const char *tag, device_t *, u32 clock) : device_t(tag, clock) {}
	void set_icountptr(int &) {}
	template <typename T> state_entry &state_add(int, const char *, T &) { return m_state; }
	address_space &space(int index)
	{
		for (auto &s : m_spaces)
			if (s.first == index)
				return *s.second;
		for (const auto &cfg : memory_space_config())
		{
			if (cfg.first != index)
				continue;
			const size_t units = (cfg.second->data_width == 32) ? (size_t(1) << cfg.second->addr_width) : m_shim_data_ram_bytes;
			m_spaces.emplace_back(index, std::make_unique<address_space>(cfg.second->data_width, units));
			return *m_spaces.back().second;
		}
		fatalerror("shim: no address space %d\n", index);
	}
	virtual u32 execute_min_cycles() const noexcept { return 1; }
	virtual u32 execute_max_cycles() const noexcept { return 1; }
	virtual void execute_run() {}
	virtual space_config_vector memory_space_config() const { return {}; }
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }

	size_t m_shim_data_ram_bytes = 0x40000;   // set before device_start(); 0 = no external RAM
private:
	state_entry m_state;
	std::vector<std::pair<int, std::unique_ptr<address_space>>> m_spaces;
};

class sound_stream
{
public:
	using sample_t = float;
	virtual ~sound_stream() = default;
	virtual sample_t get(int ch, int idx) const = 0;
	virtual void put(int ch, int idx, sample_t v) = 0;
	virtual int samples() const = 0;
	virtual u32 sample_rate() const = 0;
};

class device_sound_interface
{
public:
	device_sound_interface(const machine_config &, device_t &) {}
	virtual ~device_sound_interface() = default;
	sound_stream *stream_alloc(int, int, int, int = 0) { return nullptr; }
	virtual void sound_stream_update(sound_stream &) = 0;
};
