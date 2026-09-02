// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tca8418.h"

namespace thicket {

bool Tca8418::decode(uint8_t raw, KeyEvent& ev) {
	// SCPS215G 8.3.1.3: bit 7 is press(1)/release(0), bits [6:0] are the key
	// number from the Key Event Table. The datasheet's worked example is the
	// check: key 1 press = 0x81, key 32 press = 0xA0, key 23 release = 0x17.
	if (raw == 0x00) return false;               // FIFO empty sentinel

	const uint8_t key = (uint8_t)(raw & 0x7F);
	if (key == 0) return false;
	if (key >= TCA_GPI_FIRST) return false;      // GPI event, not a matrix key
	if (key > TCA_ROWS * TCA_COLS) return false; // outside the 80-key table

	ev.row     = (uint8_t)((key - 1) / TCA_COLS);
	ev.col     = (uint8_t)((key - 1) % TCA_COLS);
	ev.pressed = (raw & 0x80) != 0;
	return true;
}

bool Tca8418::begin(uint8_t rows, uint8_t cols) {
	if (rows == 0 || rows > TCA_ROWS || cols == 0 || cols > TCA_COLS) {
		return false;
	}

	// Claim only the rectangle we actually use. Every other pin stays GPIO, so
	// a half-populated matrix does not scan columns that are not wired.
	const uint8_t row_mask  = (uint8_t)((1u << rows) - 1u);
	const uint8_t col_mask1 = (uint8_t)((cols >= 8) ? 0xFF : ((1u << cols) - 1u));
	const uint8_t col_mask2 = (uint8_t)((cols > 8) ? ((1u << (cols - 8)) - 1u) : 0x00);

	if (!_bus.write_reg(TCA_KP_GPIO1, row_mask))  return false;
	if (!_bus.write_reg(TCA_KP_GPIO2, col_mask1)) return false;
	if (!_bus.write_reg(TCA_KP_GPIO3, col_mask2)) return false;

	// Debounce stays ENABLED (0 = enabled). The part debounces in 50 µs, two
	// orders of magnitude below its own 25 ms scan, so there is nothing to gain
	// by turning it off and a bouncing dome to lose.
	if (!_bus.write_reg(TCA_DEBOUNCE_DIS1, 0x00)) return false;

	// Overflow reporting needs BOTH bits, per the Overflow Errata (8.6.4.1):
	// "For overflow to be enabled, both Bit_3 and Bit_5 ... must be set High.
	// If only Bit_3 set high, no overflow interrupt is generated." Setting one
	// and believing overflow is covered is the trap this errata exists for.
	const uint8_t cfg = (uint8_t)(TCA_CFG_KE_IEN |
	                              TCA_CFG_OVR_FLOW_IEN |
	                              TCA_CFG_OVR_FLOW_M);
	if (!_bus.write_reg(TCA_CFG, cfg)) return false;

	// Clear anything latched from before we were configured.
	uint8_t stat = 0;
	if (_bus.read_reg(TCA_INT_STAT, stat) && stat != 0) {
		_bus.write_reg(TCA_INT_STAT, stat);
	}
	return true;
}

size_t Tca8418::poll(KeyEvent* out, size_t max) {
	if (out == nullptr || max == 0) return 0;

	uint8_t stat = 0;
	if (!_bus.read_reg(TCA_INT_STAT, stat)) return 0;

	if (stat & TCA_INT_OVR_FLOW) _overflowed = true;

	// ⚠ CAD_INT is read and DISCARDED, deliberately.
	//
	// The CAD Interrupt Errata (SCPS215G 8.6.3) states that keys 1+11, 1+21 and
	// 21+1+11 raise CAD_INT falsely, and that there is NO workaround. Keys 1,
	// 11 and 21 are R0C0, R1C0 and R2C0, so two ordinary keys held in column
	// zero during normal two-key rollover, which our input layer explicitly
	// permits, will assert it. Treating CAD_INT as meaningful would turn
	// ordinary typing into a spurious Ctrl-Alt-Delete.
	//
	// We do not use the feature at all, so this costs nothing. It is written
	// down because "why is CAD ignored" is otherwise a reasonable question with
	// an unreasonable answer buried in an errata section.
	(void)TCA_INT_CAD;

	size_t n = 0;
	if (stat & TCA_INT_K) {
		// Read the count, then drain. The count bounds the loop so a stuck bus
		// returning a constant non-zero byte cannot spin here forever.
		uint8_t ec = 0;
		if (!_bus.read_reg(TCA_KEY_LCK_EC, ec)) return 0;
		uint8_t pending = (uint8_t)(ec & 0x0F);

		while (pending > 0 && n < max) {
			// Always KEY_EVENT_A: the FIFO shifts down by one on every read
			// (SCPS215G 8.3.1.3 step 3), so reading B..J directly would replay
			// events that have already moved.
			uint8_t raw = 0;
			if (!_bus.read_reg(TCA_KEY_EVENT_A, raw)) break;
			if (raw == 0x00) break;          // documented empty marker

			KeyEvent ev;
			if (decode(raw, ev)) out[n++] = ev;
			// A GPI or out-of-range event still consumed a FIFO slot, so the
			// counter decrements whether or not we kept it.
			--pending;
		}
	}

	// Clear by writing the bits back, per the register description: "Requires
	// writing a 1 to clear interrupts."
	if (stat != 0) _bus.write_reg(TCA_INT_STAT, stat);
	return n;
}

}  // namespace thicket
