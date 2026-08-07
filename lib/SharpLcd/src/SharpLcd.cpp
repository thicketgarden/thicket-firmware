// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SharpLcd.h"

#include <string.h>

namespace thicket {

uint8_t SharpLcd::reverse_bits(uint8_t v) {
	v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
	v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
	v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
	return v;
}

void SharpLcd::mark_dirty(uint16_t y) { _dirty[y >> 3] |= (uint8_t)(1u << (y & 7)); }
bool SharpLcd::is_dirty(uint16_t y) const { return (_dirty[y >> 3] >> (y & 7)) & 1u; }

uint16_t SharpLcd::dirty_lines() const {
	uint16_t n = 0;
	for (uint16_t y = 0; y < LCD_HEIGHT; ++y) if (is_dirty(y)) ++n;
	return n;
}

void SharpLcd::fill_white() {
	// 1 = white. Confirmed against the panel's own all-clear, which writes white.
	memset(_fb, 0xFF, LCD_FB_BYTES);
	memset(_dirty, 0xFF, sizeof(_dirty));
}

void SharpLcd::set_pixel(uint16_t x, uint16_t y, bool black) {
	if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
	// Bit 7 is the leftmost pixel of the byte: it goes out first on an
	// MSB-first bus, and the panel takes D1 first.
	uint8_t* b = &_fb[(uint32_t)y * LCD_LINE_BYTES + (x >> 3)];
	const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
	const uint8_t before = *b;
	if (black) *b = (uint8_t)(*b & ~mask);
	else       *b = (uint8_t)(*b | mask);
	if (*b != before) mark_dirty(y);
}

bool SharpLcd::get_pixel(uint16_t x, uint16_t y) const {
	if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return false;
	const uint8_t b = _fb[(uint32_t)y * LCD_LINE_BYTES + (x >> 3)];
	return (b & (uint8_t)(0x80u >> (x & 7))) == 0;   // 0 = black
}

uint16_t SharpLcd::flush() {
	const uint16_t n = dirty_lines();
	if (n == 0) return 0;

	// Multi-line update, 6-5-2: mode byte, then per line an address byte, 50
	// data bytes and 8 trailing dummy clocks. The last line takes 16 instead.
	// The trailing byte of one line doubles as the gap before the next, so a
	// single dummy byte per line is emitted and one extra at the end.
	_bus.select(true);

	uint8_t cmd = LCD_M0_WRITE;
	if (_vcom) cmd |= LCD_M1_VCOM;
	_bus.write(&cmd, 1);

	for (uint16_t y = 0; y < LCD_HEIGHT; ++y) {
		if (!is_dirty(y)) continue;
		const uint8_t addr = line_address((uint16_t)(y + 1));   // lines are 1-based
		_bus.write(&addr, 1);
		_bus.write(&_fb[(uint32_t)y * LCD_LINE_BYTES], LCD_LINE_BYTES);
		const uint8_t gap = 0x00;
		_bus.write(&gap, 1);
	}

	const uint8_t trailer = 0x00;
	_bus.write(&trailer, 1);
	_bus.select(false);

	memset(_dirty, 0, sizeof(_dirty));
	return n;
}

void SharpLcd::toggle_vcom() {
	_vcom = !_vcom;
	// Display mode, 6-5-3: mode byte plus at least 13 dummy clocks. Sends no
	// pixel data and leaves the image untouched.
	uint8_t buf[2] = { (uint8_t)(_vcom ? LCD_M1_VCOM : 0x00), 0x00 };
	_bus.select(true);
	_bus.write(buf, 2);
	_bus.select(false);
}

void SharpLcd::clear() {
	uint8_t buf[2] = { (uint8_t)(LCD_M2_CLEAR | (_vcom ? LCD_M1_VCOM : 0x00)), 0x00 };
	_bus.select(true);
	_bus.write(buf, 2);
	_bus.select(false);

	memset(_fb, 0xFF, LCD_FB_BYTES);
	memset(_dirty, 0, sizeof(_dirty));   // panel and buffer now agree
}

}  // namespace thicket
