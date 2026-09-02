// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Renders three UI proposals to screens/*.pbm for comparison.
// Build: pio run -e proposals

#include "SharpLcd.h"
#include "VirtualPanel.h"

#include <stdio.h>
#include <string.h>
#include <string>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

// --- helpers ---------------------------------------------------------------

static void frame(SharpLcd& l, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	// Rounded box in Cozette's own corners; cells are 6x13.
	const uint16_t cw = SharpLcd::text_w(), ch = SharpLcd::text_h();
	const uint16_t cols = w / cw, rows = h / ch;
	std::string top = "╭", mid = "│", bot = "╰";
	for (uint16_t i = 1; i < cols - 1; ++i) { top += "─"; bot += "─"; }
	top += "╮"; bot += "╯";
	l.draw_text(x, y, top.c_str(), true);
	for (uint16_t r = 1; r < rows - 1; ++r) {
		l.draw_text(x, (uint16_t)(y + r * ch), "│", true);
		l.draw_text((uint16_t)(x + (cols - 1) * cw),
		            (uint16_t)(y + r * ch), "│", true);
	}
	l.draw_text(x, (uint16_t)(y + (rows - 1) * ch), bot.c_str(), true);
}

static void dither(SharpLcd& l, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint8_t level) {
	// 1 in N pixels, ordered. The only shading a 1-bit panel has.
	for (uint16_t yy = y; yy < y + h; ++yy)
		for (uint16_t xx = x; xx < x + w; ++xx) {
			bool on = false;
			if (level == 1) on = ((xx + yy) % 4 == 0);
			else if (level == 2) on = ((xx + yy) % 2 == 0);
			else if (level == 3) on = !((xx + yy) % 4 == 0);
			if (on) l.set_pixel(xx, yy, true);
		}
}

// Solid bar with knocked-out text. On 1-bit there is no third value, so
// dither behind text competes with the glyph and both lose.
static void bar(SharpLcd& l, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	l.fill_rect(x, y, w, h, true);
}

static void save(VirtualPanel& p, const char* name) {
	char path[128];
	snprintf(path, sizeof(path), "screens/%s.pbm", name);
	p.write_pbm(path, 2);
	printf("  %s\n", path);
}

// ===========================================================================
// A, THE CREATURE. A living thing that reacts to real events.
// The mesh has a body; the messages are what it brings you.
// ===========================================================================

static void creature_body(SharpLcd& l, uint16_t cx, uint16_t cy, bool listening,
                          uint8_t breath) {
	// A fruiting body: broad cap, leaning stem, flared base. Asymmetric on
	// purpose - the design language says growth, not erosion.
	const int r = 40 + breath;

	// stem first, so the cap overlaps it
	for (int yy = 0; yy < 46; ++yy) {
		const int lean = yy / 7;
		const int wdt = (yy > 36) ? 5 + (yy - 36) : 6 - yy / 12;
		for (int xx = -wdt; xx <= wdt; ++xx)
			l.set_pixel((uint16_t)(cx + xx + lean), (uint16_t)(cy + yy), true);
	}

	// cap: a squashed dome, heavier on one side
	for (int yy = -r; yy <= 10; ++yy) {
		for (int xx = -r - 8; xx <= r + 8; ++xx) {
			const float fx = (float)(xx + yy / 5) / (float)(r + 8);
			const float fy = (float)yy / (float)(r * 0.86f);
			if (fx * fx + fy * fy <= 1.0f && yy <= 8)
				l.set_pixel((uint16_t)(cx + xx), (uint16_t)(cy + yy), true);
		}
	}

	// gills: a lighter band under the cap edge, knocked out in dither
	for (int yy = 2; yy <= 9; ++yy)
		for (int xx = -r + 4; xx <= r - 2; ++xx)
			if (((xx + yy) & 3) == 0)
				l.set_pixel((uint16_t)(cx + xx), (uint16_t)(cy + yy), false);

	// eyes
	for (int e = 0; e < 2; ++e) {
		const int ex = cx - 13 + e * 26;
		if (listening) {
			for (int yy = -20; yy <= -12; ++yy)
				for (int xx = -3; xx <= 3; ++xx)
					l.set_pixel((uint16_t)(ex + xx), (uint16_t)(cy + yy), false);
			// a highlight, so it reads as an eye rather than a hole
			l.set_pixel((uint16_t)(ex + 1), (uint16_t)(cy - 18), true);
			l.set_pixel((uint16_t)(ex + 2), (uint16_t)(cy - 18), true);
		} else {
			for (int xx = -4; xx <= 4; ++xx)
				l.set_pixel((uint16_t)(ex + xx), (uint16_t)(cy - 16), false);
		}
	}
}

static void proposal_a(VirtualPanel& p, SharpLcd& l) {
	// resting
	l.fill_white();
	creature_body(l, 196, 92, true, 4);
	l.draw_text(140, 166, "everything is quiet", true);
	l.draw_text(158, 186, "2 friends near", true);
	l.draw_text(3, 224, "thicket", true);
	l.draw_text(340, 224, "4 days", true);
	l.flush(); save(p, "a1-resting");

	// someone wrote
	l.fill_white();
	creature_body(l, 108, 96, true, 2);
	frame(l, 230, 42, 160, 78);
	l.draw_text(242, 52, "mara", true);
	l.draw_text(242, 70, "are you coming", true);
	l.draw_text(242, 84, "down to the river", true);
	l.draw_text(242, 100, "♡ just now", true);
	l.draw_text(104, 186, "mara brought you something", true);
	l.draw_text(3, 224, "thicket", true);
	l.draw_text(316, 224, "♥ 2 unread", true);
	l.flush(); save(p, "a2-message");

	// composing
	l.fill_white();
	creature_body(l, 66, 70, true, 0);
	l.draw_text(120, 30, "to mara", true);
	frame(l, 8, 120, 384, 66);
	l.draw_text(20, 132, "on my way, ten minutes█", true);
	l.draw_text(20, 160, "♡ she is close by", true);
	l.draw_text(3, 224, "thicket", true);
	l.draw_text(280, 224, "→ send    ← back", true);
	l.flush(); save(p, "a3-composing");
}

// ===========================================================================
// B, THE POD. Framed, boxed, cozy. A little machine you carry.
// ===========================================================================

static void proposal_b(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	frame(l, 0, 0, 400, 234);
	bar(l, 6, 4, 388, 17);
	l.draw_text(12, 6, "thicket", false);
	l.draw_text(316, 6, "♥ ♥ ♡", false);

	l.draw_text(18, 40, "●  mara", true);
	l.draw_text(18, 54, "   are you coming down to the river", true);
	l.draw_text(300, 54, "just now", true);

	l.draw_text(18, 80, "○  sam", true);
	l.draw_text(18, 94, "   found the thing you left", true);
	l.draw_text(312, 94, "1 hour", true);

	l.draw_text(18, 120, "○  base", true);
	l.draw_text(18, 134, "   weather turning tonight", true);
	l.draw_text(318, 134, "3 hours", true);

	l.draw_text(18, 176, "│ 2 waiting for you", true);
	l.draw_text(18, 194, "│ everyone nearby is well", true);
	l.draw_text(18, 212, "→ open   ↓ next   ♡ new", true);
	l.flush(); save(p, "b1-list");

	l.fill_white();
	frame(l, 0, 0, 400, 234);
	bar(l, 6, 4, 388, 17);
	l.draw_text(12, 6, "mara", false);
	l.draw_text(316, 6, "close by", false);

	l.draw_text(18, 40, "are you coming down", true);
	l.draw_text(18, 54, "to the river", true);
	l.draw_text(320, 54, "♡ 9:14", true);

	l.draw_text(150, 88, "on my way, ten minutes", true);
	l.draw_text(320, 102, "✓ read", true);

	l.draw_text(18, 136, "bring the small lamp", true);
	l.draw_text(320, 136, "♡ 9:20", true);

	frame(l, 8, 168, 384, 40);
	l.draw_text(20, 180, "█", true);
	l.draw_text(18, 212, "→ send   ← back   ♡ friends", true);
	l.flush(); save(p, "b2-conversation");

	l.fill_white();
	frame(l, 0, 0, 400, 234);
	bar(l, 6, 4, 388, 17);
	l.draw_text(12, 6, "thicket", false);
	frame(l, 58, 68, 284, 94);
	l.draw_text(76, 84, "nobody can hear you here", true);
	l.draw_text(76, 108, "walk somewhere higher, or", true);
	l.draw_text(76, 122, "wait, it keeps trying", true);
	l.draw_text(76, 142, "♡ 3 messages waiting to go", true);
	l.draw_text(18, 212, "← back", true);
	l.flush(); save(p, "b3-alone");
}

// ===========================================================================
// C, THE TAPE. Text-forward, warm, almost no chrome. A continuous ribbon.
// ===========================================================================

static void proposal_c(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	l.draw_text(3, 4, "thicket", true);
	l.draw_text(292, 4, "♥ ♥ ♡   4 days", true);
	dither(l, 0, 20, 400, 2, 2);

	int y = 34;
	const char* tape[][3] = {
		{"mara",  "are you coming down to the river", "9:14"},
		{"you",   "on my way, ten minutes",           "9:15"},
		{"sam",   "found the thing you left",         "8:02"},
		{"you",   "keep it, i will come by",          "8:04"},
		{"base",  "weather turning tonight",          "6:40"},
	};
	for (auto& row : tape) {
		const bool mine = (strcmp(row[0], "you") == 0);
		l.draw_text(mine ? 40 : 3, (uint16_t)y, row[0], true);
		l.draw_text(mine ? 88 : 51, (uint16_t)y, row[1], true);
		l.draw_text(352, (uint16_t)y, row[2], true);
		if (mine) l.draw_text(340, (uint16_t)y, "✓", true);
		y += 16;
	}

	dither(l, 0, 196, 400, 2, 2);
	l.draw_text(3, 206, "→", true);
	l.draw_text(21, 206, "bring the small lamp█", true);
	l.flush(); save(p, "c1-tape");

	l.fill_white();
	l.draw_text(3, 4, "thicket", true);
	dither(l, 0, 20, 400, 2, 2);
	l.draw_text(3, 40, "mara", true);
	l.draw_text(3, 56, "sam", true);
	l.draw_text(3, 72, "base", true);
	l.draw_text(3, 88, "the orchard relay", true);
	for (int i = 0; i < 4; ++i) {
		const int bars = 4 - i;
		for (int b = 0; b < bars; ++b)
			l.fill_rect((uint16_t)(320 + b * 8), (uint16_t)(46 + i * 16 - b * 2),
			            5, (uint16_t)(3 + b * 2), true);
	}
	l.draw_text(3, 130, "everyone here is well.", true);
	l.draw_text(3, 146, "the orchard relay is passing", true);
	l.draw_text(3, 162, "messages along for you.", true);
	dither(l, 0, 196, 400, 2, 2);
	l.draw_text(3, 206, "↓ next   → open   ♡ new", true);
	l.flush(); save(p, "c2-friends");
}

int main() {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	printf("[proposals] rendering\n");
	proposal_a(panel, lcd);
	proposal_b(panel, lcd);
	proposal_c(panel, lcd);
	printf("[proposals] done\n");
	return 0;
}
