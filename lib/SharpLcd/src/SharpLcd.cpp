// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SharpLcd.h"
#include "CozetteFont.h"
#include "CozetteBig.h"
#include "TamzenFont.h"

#include <string.h>

namespace thicket {

uint8_t SharpLcd::text_w() { return FONT_W; }
uint8_t SharpLcd::text_h() { return FONT_H; }

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

// Returns the glyph rows for a codepoint, or nullptr.
static const uint8_t* glyph_for(uint32_t cp) {
	if (cp >= FONT_FIRST && cp <= FONT_LAST) return FONT[cp - FONT_FIRST];
	uint16_t lo = 0, hi = FONT_EXTRA_COUNT;
	while (lo < hi) {
		const uint16_t mid = (uint16_t)((lo + hi) / 2);
		if (FONT_EXTRA[mid].cp == cp) return FONT_EXTRA[mid].rows;
		if (FONT_EXTRA[mid].cp < cp) lo = (uint16_t)(mid + 1);
		else hi = mid;
	}
	return nullptr;
}

// Minimal UTF-8: advances `s` past one codepoint.
uint32_t next_cp(const char*& s);
uint32_t next_cp(const char*& s) {
	const uint8_t c = (uint8_t)*s++;
	if (c < 0x80) return c;
	uint32_t cp; int extra;
	if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
	else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
	else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
	else return 0xFFFD;
	while (extra-- > 0) {
		if ((*s & 0xC0) != 0x80) return 0xFFFD;
		cp = (cp << 6) | (uint32_t)(*s++ & 0x3F);
	}
	return cp;
}

uint16_t SharpLcd::draw_text(uint16_t x, uint16_t y, const char* s, bool black) {
	// Cozette is monospaced and its 6px advance already includes side bearing,
	// so glyphs butt up with no extra column.
	while (*s) {
		const uint32_t cp = next_cp(s);
		const uint8_t* g = glyph_for(cp);
		if (g) {
			for (uint8_t row = 0; row < FONT_H; ++row) {
				const uint8_t bits = g[row];
				if (!bits) continue;
				// FONT_INK_W, not FONT_W: box & block glyphs are 7 wide
				// against a 6px advance so neighbouring cells touch & rules
				// join. Drawing only the advance clips that column & breaks
				// every rule into dashes.
				for (uint8_t col = 0; col < FONT_INK_W; ++col) {
					if (bits & (uint8_t)(0x80u >> col))
						set_pixel((uint16_t)(x + col), (uint16_t)(y + row), black);
				}
			}
		}
		x = (uint16_t)(x + FONT_W);
	}
	return x;
}

uint16_t SharpLcd::draw_text_scaled(uint16_t x, uint16_t y, const char* s,
                                    bool black, uint8_t scale) {
	if (scale <= 1) return draw_text(x, y, s, black);
	while (*s) {
		const uint32_t cp = next_cp(s);
		const uint8_t* g = glyph_for(cp);
		if (g) {
			for (uint8_t row = 0; row < FONT_H; ++row) {
				const uint8_t bits = g[row];
				if (!bits) continue;
				for (uint8_t col = 0; col < FONT_INK_W; ++col) {
					if (!(bits & (uint8_t)(0x80u >> col))) continue;
					for (uint8_t sy = 0; sy < scale; ++sy)
						for (uint8_t sx = 0; sx < scale; ++sx)
							set_pixel((uint16_t)(x + col * scale + sx),
							          (uint16_t)(y + row * scale + sy), black);
				}
			}
		}
		x = (uint16_t)(x + FONT_W * scale);
	}
	return x;
}

void SharpLcd::fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
	for (uint16_t yy = y; yy < y + h; ++yy)
		for (uint16_t xx = x; xx < x + w; ++xx) set_pixel(xx, yy, black);
}

void SharpLcd::draw_hline(uint16_t x, uint16_t y, uint16_t w, bool black) {
	for (uint16_t xx = x; xx < x + w; ++xx) set_pixel(xx, y, black);
}

uint16_t SharpLcd::flush() {
	const uint16_t n = dirty_lines();
	if (n == 0) return 0;

	// Multi-line update, 6-5-2: mode byte, then per line an address byte, 50
	// data bytes & 8 trailing dummy clocks. The last line takes 16 instead.
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
	// pixel data & leaves the image untouched.
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
	memset(_dirty, 0, sizeof(_dirty));   // panel & buffer now agree
}

}  // namespace thicket

namespace thicket {
namespace {
const uint16_t* big_glyph(uint32_t cp) {
	uint16_t lo = 0, hi = BIGFONT_COUNT;
	while (lo < hi) {
		const uint16_t mid = (uint16_t)((lo + hi) / 2);
		if (BIGFONT[mid].cp == cp) return BIGFONT[mid].rows;
		if (BIGFONT[mid].cp < cp) lo = (uint16_t)(mid + 1);
		else hi = mid;
	}
	return nullptr;
}
}  // namespace

bool SharpLcd::big_has(uint32_t cp) { return big_glyph(cp) != nullptr; }
bool SharpLcd::has_glyph(uint32_t cp) { return glyph_for(cp) != nullptr; }

// 4x4 Bayer, scaled to 0..255. Ordered rather than error-diffused because a
// page re-flows on every scroll step: a diffused pattern would crawl as the
// window moves, while an ordered one is a pure function of position.
static const uint8_t BAYER4[16] = {
	  8, 136,  40, 168,
	200,  72, 232, 104,
	 56, 184,  24, 152,
	248, 120, 216,  88,
};

bool SharpLcd::dither_on(uint16_t x, uint16_t y, uint8_t level) {
	return level <= BAYER4[(y & 3) * 4 + (x & 3)];
}

void SharpLcd::fill_dither(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t level) {
	if (level >= 249) return;                       // nothing to draw
	for (uint16_t yy = y; yy < (uint32_t)y + h && yy < LCD_HEIGHT; ++yy)
		for (uint16_t xx = x; xx < (uint32_t)x + w && xx < LCD_WIDTH; ++xx)
			if (dither_on(xx, yy, level)) set_pixel(xx, yy, true);
}
uint32_t SharpLcd::next_codepoint(const char*& s) { return next_cp(s); }
uint8_t SharpLcd::big_text_w() { return BIGFONT_W; }
uint8_t SharpLcd::big_text_h() { return BIGFONT_H; }

uint16_t SharpLcd::draw_text_big(uint16_t x, uint16_t y, const char* s, bool black) {
	while (*s) {
		const uint32_t cp = next_cp(s);
		const uint16_t* g = big_glyph(cp);
		if (g) {
			for (uint8_t row = 0; row < BIGFONT_H; ++row) {
				const uint16_t bits = g[row];
				if (!bits) continue;
				for (uint8_t col = 0; col < BIGFONT_INK_W; ++col)
					if (bits & (uint16_t)(0x8000u >> col))
						set_pixel((uint16_t)(x + col), (uint16_t)(y + row), black);
			}
		}
		x = (uint16_t)(x + BIGFONT_W);
	}
	return x;
}

}  // namespace thicket

namespace thicket {
namespace {
const uint8_t* bold_glyph(uint32_t cp, SharpLcd::BoldFace face, uint8_t& adv, uint8_t& cell, uint8_t& asc) {
	// One binary search per face. The tables are codepoint-sorted by the
	// generator, ASCII then Latin-1.
	switch (face) {
		case SharpLcd::BOLD_INLINE: {
			adv = BOLD6_W; cell = BOLD6_H; asc = BOLD6_ASCENT;
			uint16_t lo = 0, hi = BOLD6_COUNT;
			while (lo < hi) { uint16_t m = (lo + hi) / 2;
				if (BOLD6[m].cp == cp) return BOLD6[m].rows;
				if (BOLD6[m].cp < cp) lo = m + 1; else hi = m; }
			return nullptr; }
		case SharpLcd::BOLD_H3: {
			adv = BOLD7_W; cell = BOLD7_H; asc = BOLD7_ASCENT;
			uint16_t lo = 0, hi = BOLD7_COUNT;
			while (lo < hi) { uint16_t m = (lo + hi) / 2;
				if (BOLD7[m].cp == cp) return BOLD7[m].rows;
				if (BOLD7[m].cp < cp) lo = m + 1; else hi = m; }
			return nullptr; }
		default: {
			adv = BOLD8_W; cell = BOLD8_H; asc = BOLD8_ASCENT;
			uint16_t lo = 0, hi = BOLD8_COUNT;
			while (lo < hi) { uint16_t m = (lo + hi) / 2;
				if (BOLD8[m].cp == cp) return BOLD8[m].rows;
				if (BOLD8[m].cp < cp) lo = m + 1; else hi = m; }
			return nullptr; }
	}
}
}

uint8_t SharpLcd::bold_w(BoldFace f) { return f == BOLD_INLINE ? BOLD6_W : f == BOLD_H3 ? BOLD7_W : BOLD8_W; }
uint8_t SharpLcd::bold_h(BoldFace f) { return f == BOLD_INLINE ? BOLD6_H : f == BOLD_H3 ? BOLD7_H : BOLD8_H; }
uint8_t SharpLcd::bold_ascent(BoldFace f) { return f == BOLD_INLINE ? BOLD6_ASCENT : f == BOLD_H3 ? BOLD7_ASCENT : BOLD8_ASCENT; }

bool SharpLcd::bold_has(uint32_t cp, BoldFace face) {
	uint8_t a, c, s; return bold_glyph(cp, face, a, c, s) != nullptr;
}

uint16_t SharpLcd::draw_text_bold(uint16_t x, uint16_t y, const char* str, BoldFace face, bool black) {
	while (*str) {
		const uint32_t cp = next_cp(str);
		uint8_t adv, cell, asc;
		const uint8_t* g = bold_glyph(cp, face, adv, cell, asc);
		if (g) {
			for (uint8_t row = 0; row < cell; ++row) {
				const uint8_t bits = g[row];
				if (!bits) continue;
				for (uint8_t col = 0; col < 8; ++col)
					if (bits & (uint8_t)(0x80u >> col))
						set_pixel((uint16_t)(x + col), (uint16_t)(y + row), black);
			}
		}
		x = (uint16_t)(x + adv);
	}
	return x;
}

}  // namespace thicket
