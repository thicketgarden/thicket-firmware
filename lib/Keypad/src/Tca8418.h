// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TCA8418 keypad scanner: FIFO decode & configuration.
//
// Feeds `thicket::InputLayer`, which owns the one-shot modifier logic and the
// ghost guard & deliberately knows nothing about this chip. This half knows
// nothing about modifiers in return: it turns register bytes into
// (row, column, pressed) & stops there.
//
// SOURCE. Every register address, bit position & encoding here was read from
// TI's datasheet SCPS215G (September 2009, revised June 2018), sections 8.3
// and 8.6, rather than from any secondhand description.
//
// That distinction matters here. A summary of the same PDF, produced by a model
// rather than read, got the slave address and the event encoding roughly right
// and then placed KP_GPIO1 at 0x08 (it's 0x1D), GPI_EM1 at 0x0B (0x20),
// DEBOUNCE_DIS1 at 0x0F (0x29), and called the FIFO four events deep (it is
// ten). A driver built on it would have written GPIO event masks while trying
// to select the keypad matrix, and the failure would have looked like broken
// hardware.
//
// NO BUS DEPENDENCY. The chip is reached through an abstract Bus, so all of
// this runs and is tested on the host with no I2C and no keyboard, the same
// reason InputLayer could be written before either existed.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace thicket {

// --- registers, SCPS215G Table 9 -----------------------------------------
enum : uint8_t {
	TCA_CFG            = 0x01,
	TCA_INT_STAT       = 0x02,
	TCA_KEY_LCK_EC     = 0x03,
	TCA_KEY_EVENT_A    = 0x04,   // FIFO head; A..J = 0x04..0x0D, ten deep
	TCA_KP_GPIO1       = 0x1D,   // ROW0-7   1 = part of the keypad matrix
	TCA_KP_GPIO2       = 0x1E,   // COL0-7
	TCA_KP_GPIO3       = 0x1F,   // COL8-9
	TCA_GPI_EM1        = 0x20,
	TCA_GPI_EM2        = 0x21,
	TCA_GPI_EM3        = 0x22,
	TCA_DEBOUNCE_DIS1  = 0x29,
};

// --- CFG bits, SCPS215G Table 11 -----------------------------------------
enum : uint8_t {
	TCA_CFG_AI           = 1 << 7,
	TCA_CFG_GPI_E_CFG    = 1 << 6,
	TCA_CFG_OVR_FLOW_M   = 1 << 5,
	TCA_CFG_INT_CFG      = 1 << 4,
	TCA_CFG_OVR_FLOW_IEN = 1 << 3,
	TCA_CFG_K_LCK_IEN    = 1 << 2,
	TCA_CFG_GPI_IEN      = 1 << 1,
	TCA_CFG_KE_IEN       = 1 << 0,
};

// --- INT_STAT bits, SCPS215G Table 10 ------------------------------------
enum : uint8_t {
	TCA_INT_CAD      = 1 << 4,
	TCA_INT_OVR_FLOW = 1 << 3,
	TCA_INT_K_LCK    = 1 << 2,
	TCA_INT_GPI      = 1 << 1,
	TCA_INT_K        = 1 << 0,
};

// 7-bit address 0b0110100. SCPS215G Table 8.
static const uint8_t TCA8418_ADDR = 0x34;

// Matrix geometry is fixed by the part: 8 rows x 10 columns = 80 keys, and the
// key numbering runs across columns first. Confirmed independently by the
// datasheet's own Control-Alt-Delete note, which names keys 1, 11 & 21,
// exactly R0C0, R1C0, R2C0 under a stride of ten. That cross-check is why the
// stride isn't a guess.
static const uint8_t TCA_ROWS = 8;
static const uint8_t TCA_COLS = 10;

// Key numbers at or above this are GPI events (R0-R7 = 97-104, C0-C9 =
// 105-114), not matrix presses. SCPS215G Tables 2 & 3.
static const uint8_t TCA_GPI_FIRST = 97;

struct KeyEvent {
	uint8_t row;
	uint8_t col;
	bool    pressed;
};

// Minimal I2C surface, so the driver can be exercised without a bus.
class Tca8418Bus {
public:
	virtual ~Tca8418Bus() {}
	virtual bool write_reg(uint8_t reg, uint8_t value) = 0;
	virtual bool read_reg(uint8_t reg, uint8_t& value) = 0;
};

class Tca8418 {
public:
	explicit Tca8418(Tca8418Bus& bus) : _bus(bus) {}

	// Configure `rows` x `cols` of the matrix & enable key-event interrupts.
	// Anything outside that rectangle is left as GPIO.
	bool begin(uint8_t rows, uint8_t cols);

	// Drain the FIFO. Returns the number of events written to `out`, at most
	// `max`. Safe to call whether or not INT is asserted.
	size_t poll(KeyEvent* out, size_t max);

	// Overflow is reported, never swallowed: by the time the chip raises it,
	// events have ALREADY been dropped (SCPS215G 8.6.4.3), so a caller that
	// cares about keys not going missing needs to know.
	bool overflowed() const { return _overflowed; }
	void clear_overflow() { _overflowed = false; }

	// Decode one FIFO byte. Exposed for testing, and because it's the single
	// place the encoding is interpreted.
	static bool decode(uint8_t raw, KeyEvent& ev);

private:
	Tca8418Bus& _bus;
	bool _overflowed = false;
};

}  // namespace thicket
