// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sharp LS027B7DH01 memory LCD driver.
// All values from Sharp spec LCP-2110015A (Rev. Jun 2010), sections 4, 6-3,
// 6-5, 6-6.
//
// Three non-obvious traits of this panel:
//   - SCS is active HIGH (6-5-1, 6-3-2).
//   - Gate address is sent LSB first (6-6), so bit-reverse it on an MSB-first
//     bus. Pixel data is a bit sequence, not a value, and needs no reversal.
//   - VCOM must toggle at 0.5-10 Hz (6-3-1) even on a static image, or the
//     cell takes a DC bias.
//
// The panel is write-only, so the framebuffer is the only copy of the screen.
// The caller supplies it; at 12,000 bytes it's the largest single RAM
// allocation in the device.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace thicket {

// Geometry, LCP-2110015A section 3.
static const uint16_t LCD_WIDTH      = 400;
static const uint16_t LCD_HEIGHT     = 240;
static const uint16_t LCD_LINE_BYTES = LCD_WIDTH / 8;                         // 50
static const uint32_t LCD_FB_BYTES   = (uint32_t)LCD_LINE_BYTES * LCD_HEIGHT; // 12000

// Mode bits, 6-5-1..6-5-4. M0 is sent first, so it's bit 7 on an MSB-first bus.
static const uint8_t LCD_M0_WRITE = 0x80;  // data update
static const uint8_t LCD_M1_VCOM  = 0x40;  // frame inversion (EXTMODE low)
static const uint8_t LCD_M2_CLEAR = 0x20;  // all clear, writes white

static const uint32_t LCD_SCLK_MAX_HZ = 2000000;  // 6-3-1, typ 1 MHz
static const uint32_t LCD_VCOM_MIN_MHZ = 500;     // 0.5 Hz in milli-hertz
static const uint32_t LCD_VCOM_MAX_HZ  = 10;

class SharpLcdBus {
public:
	virtual ~SharpLcdBus() {}
	virtual void select(bool on) = 0;   // on = SCS high
	virtual void write(const uint8_t* data, size_t len) = 0;
};

class SharpLcd {
public:
	// `framebuffer` must be >= LCD_FB_BYTES and outlive this object.
	SharpLcd(SharpLcdBus& bus, uint8_t* framebuffer)
		: _bus(bus), _fb(framebuffer), _vcom(false) {
		for (size_t i = 0; i < sizeof(_dirty); ++i) _dirty[i] = 0;
	}

	void fill_white();

	// Out-of-range coordinates are ignored, not wrapped.
	void set_pixel(uint16_t x, uint16_t y, bool black);

	// 5x7 text. Returns the x just past the last glyph.
	uint16_t draw_text(uint16_t x, uint16_t y, const char* s, bool black = true);

	// Same glyphs at an integer scale. Advance scales with them, so a run of
	// text stays monospaced.
	uint16_t draw_text_scaled(uint16_t x, uint16_t y, const char* s,
	                          bool black = true, uint8_t scale = 1);
	void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black);
	void draw_hline(uint16_t x, uint16_t y, uint16_t w, bool black);
	bool get_pixel(uint16_t x, uint16_t y) const;

	// Sends only changed lines. Returns the number of lines sent.
	uint16_t flush();

	// Must be called at 0.5-10 Hz when EXTMODE is tied low.
	void toggle_vcom();

	// Panel clears itself white; no framebuffer transfer.
	void clear();

	bool vcom() const { return _vcom; }
	uint16_t dirty_lines() const;

	// Text cell, so callers can lay out in character units without pulling in
	// the font table.
	static uint8_t text_w();
	static uint8_t text_h();

	static uint8_t reverse_bits(uint8_t v);
	static uint8_t line_address(uint16_t line_1based) {
		return reverse_bits((uint8_t)line_1based);
	}

private:
	void mark_dirty(uint16_t y);
	bool is_dirty(uint16_t y) const;

	SharpLcdBus& _bus;
	uint8_t*     _fb;
	uint8_t      _dirty[(LCD_HEIGHT + 7) / 8];
	bool         _vcom;
};

}  // namespace thicket
