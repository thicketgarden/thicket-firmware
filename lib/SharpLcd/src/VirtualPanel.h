// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-side stand-in for the LS027B7DH01.
//
// Decodes the SPI byte stream back into an image rather than reading the
// driver's framebuffer, so it doubles as a check on the wire format: a wrong
// gate address or bit order shows up as a wrong picture.
//
// Not a substitute for the panel. Contrast, refresh artefacts & legibility at
// arm's length aren't modelled.

#pragma once

#include "SharpLcd.h"
#include <stddef.h>
#include <stdint.h>

namespace thicket {

class VirtualPanel : public SharpLcdBus {
public:
	VirtualPanel();

	void select(bool on) override;
	void write(const uint8_t* data, size_t len) override;

	// true = black.
	bool pixel(uint16_t x, uint16_t y) const;

	uint32_t lines_written() const { return _lines_written; }
	uint32_t clears() const { return _clears; }
	uint32_t vcom_toggles() const { return _vcom_toggles; }
	bool overflowed() const { return _overflowed; }
	bool vcom() const { return _vcom; }

	// Portable bitmap, 1 = black. Returns false if the file can't be opened.
	bool write_pbm(const char* path, uint8_t scale = 1) const;

	void reset_counters();

private:
	void apply_transaction();

	uint8_t  _img[LCD_FB_BYTES];        // 1 = white, matching the panel
	// A full-screen flush is mode + 240*(addr+50+gap) + trailer.
	static const size_t TX_MAX =
		1 + (size_t)LCD_HEIGHT * (1 + LCD_LINE_BYTES + 1) + 1;
	uint8_t  _tx[TX_MAX];               // bytes of the current transaction
	size_t   _tx_len;
	bool     _selected;
	bool     _vcom;
	uint32_t _lines_written;
	uint32_t _clears;
	uint32_t _vcom_toggles;
	bool     _overflowed;
};

}  // namespace thicket
