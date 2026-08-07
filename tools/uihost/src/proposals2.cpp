// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// B and C, second pass: wheel-first navigation and heavier organic chrome.
// Every screen is a list with exactly one selection; the wheel moves it and
// the axial press commits.

#include "SharpLcd.h"
#include "VirtualPanel.h"

#include <stdio.h>
#include <string.h>
#include <string>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

// --- organic chrome --------------------------------------------------------

// Rounded outline drawn in pixels rather than box glyphs, so corners can be
// softer than a 6x13 cell allows.
static void panel(SharpLcd& l, int x, int y, int w, int h, int r) {
	for (int i = r; i < w - r; ++i) {
		l.set_pixel((uint16_t)(x + i), (uint16_t)y, true);
		l.set_pixel((uint16_t)(x + i), (uint16_t)(y + h - 1), true);
	}
	for (int i = r; i < h - r; ++i) {
		l.set_pixel((uint16_t)x, (uint16_t)(y + i), true);
		l.set_pixel((uint16_t)(x + w - 1), (uint16_t)(y + i), true);
	}
	for (int a = 0; a <= r; ++a) {
		const int b = (int)(r - __builtin_sqrtf((float)(r * r - (r - a) * (r - a))));
		l.set_pixel((uint16_t)(x + b), (uint16_t)(y + a), true);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + a), true);
		l.set_pixel((uint16_t)(x + b), (uint16_t)(y + h - 1 - a), true);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + h - 1 - a), true);
	}
}

// Selection: a filled band with rounded ends. Text is knocked out of it.
static void band(SharpLcd& l, int x, int y, int w, int h) {
	for (int yy = 0; yy < h; ++yy) {
		int inset = 0;
		if (yy < 3) inset = 3 - yy;
		else if (yy >= h - 3) inset = 3 - (h - 1 - yy);
		for (int xx = inset; xx < w - inset; ++xx)
			l.set_pixel((uint16_t)(x + xx), (uint16_t)(y + yy), true);
	}
}

// Where the wheel is. A stem; the current detent is a filled bud, the rest are
// ticks. Rings this small read as diamonds, so they are drawn as marks instead.
static void rail(SharpLcd& l, int x, int y, int h, int count, int at) {
	for (int i = 0; i < h; ++i)
		if ((i & 3) != 3) l.set_pixel((uint16_t)x, (uint16_t)(y + i), true);
	if (count <= 0) return;
	for (int i = 0; i < count; ++i) {
		const int by = y + (count == 1 ? h / 2 : i * (h - 8) / (count - 1)) + 4;
		if (i == at) {
			for (int dy = -4; dy <= 4; ++dy)
				for (int dx = -4; dx <= 4; ++dx)
					if (dx * dx + dy * dy <= 16)
						l.set_pixel((uint16_t)(x + dx), (uint16_t)(by + dy), true);
		} else {
			const int dir = (i & 1) ? 1 : -1;
			for (int k = 2; k <= 5; ++k)
				l.set_pixel((uint16_t)(x + k * dir), (uint16_t)by, true);
		}
	}
}

// A divider that fades rather than rules: dense at the edges, open in the
// middle. Reads as growth rather than a cut.
static void soft_rule(SharpLcd& l, int x, int y, int w) {
	for (int i = 0; i < w; ++i) {
		const float t = (float)i / (float)w;
		const float edge = (t < 0.5f) ? t * 2.0f : (1.0f - t) * 2.0f;
		const int step = 1 + (int)(edge * 5.0f);
		if (i % step == 0) l.set_pixel((uint16_t)(x + i), (uint16_t)y, true);
	}
}

// Corner flourish: a small sprig, so the frame is grown rather than cut.
static void sprig(SharpLcd& l, int x, int y, int dir) {
	// Curved stem with three leaves. Bigger than the first attempt, which was
	// too small to read as anything but noise.
	for (int i = 0; i < 16; ++i) {
		const int sy = y - (i * i) / 26;
		l.set_pixel((uint16_t)(x + i * dir), (uint16_t)sy, true);
		if (i == 4 || i == 9 || i == 14) {
			const int up = (i == 9) ? -1 : 1;
			for (int k = 1; k <= 4; ++k) {
				const int lx = x + (i + k / 2) * dir;
				const int ly = sy + up * k;
				for (int t = 0; t < 2; ++t)
					l.set_pixel((uint16_t)(lx + t * dir), (uint16_t)ly, true);
			}
		}
	}
}

static void save(VirtualPanel& p, const char* name) {
	char path[128];
	snprintf(path, sizeof(path), "screens/%s.pbm", name);
	p.write_pbm(path, 2);
	printf("  %s\n", path);
}

// ===========================================================================
// B2 — THE POD, wheel-first. Heavy chrome, one selection, press to enter.
// ===========================================================================

static void b2(VirtualPanel& p, SharpLcd& l) {
	struct Row { const char* who; const char* what; const char* when; bool unread; };
	const Row rows[] = {
		{"mara",  "are you coming down to the river", "now",  true},
		{"sam",   "found the thing you left",         "1h",   true},
		{"base",  "weather turning tonight",          "3h",   false},
		{"orchard relay", "passing 4 messages along", "6h",   false},
		{"ilse",  "safe home, thank you",             "y'day",false},
	};
	const int SEL = 1;

	l.fill_white();
	panel(l, 2, 2, 396, 236, 10);
	sprig(l, 16, 20, 1);

	l.fill_rect(14, 10, 120, 15, true);
	l.draw_text(20, 11, "thicket", false);
	l.draw_text(300, 11, "2 new  ♥♥♡", true);
	soft_rule(l, 14, 32, 372);

	int y = 42;
	for (int i = 0; i < 5; ++i) {
		const bool sel = (i == SEL);
		if (sel) band(l, 12, y - 3, 356, 32);
		l.draw_text(24, (uint16_t)y, rows[i].unread ? "●" : "○", !sel);
		l.draw_text(42, (uint16_t)y, rows[i].who, !sel);
		l.draw_text(24, (uint16_t)(y + 14), rows[i].what, !sel);
		l.draw_text(320, (uint16_t)y, rows[i].when, !sel);
		y += 36;
	}

	rail(l, 380, 44, 170, 5, SEL);
	soft_rule(l, 14, 214, 372);
	l.draw_text(140, 222, "press to open", true);
	l.flush(); save(p, "b2a-list");

	// quick replies: the wheel-only path the bench-messenger stage asks for
	l.fill_white();
	panel(l, 2, 2, 396, 236, 10);
	l.fill_rect(14, 10, 120, 15, true);
	l.draw_text(20, 11, "to mara", false);
	soft_rule(l, 14, 32, 372);

	const char* quick[] = {"on my way", "ten minutes", "not tonight",
	                       "yes", "call me", "write something…"};
	const int QSEL = 1;
	y = 46;
	for (int i = 0; i < 6; ++i) {
		if (i == QSEL) band(l, 40, y - 4, 300, 24);
		l.draw_text(56, (uint16_t)y, quick[i], i != QSEL);
		y += 28;
	}
	rail(l, 360, 48, 168, 6, QSEL);
	soft_rule(l, 14, 214, 372);
	l.draw_text(120, 222, "press to send it", true);
	l.flush(); save(p, "b2b-quick");
}

// ===========================================================================
// C2 — THE TAPE, wheel-first. Almost no frame; the chrome is organic.
// ===========================================================================

static void c2(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();

	// A grown header: sprig, wordmark, fading rule
	sprig(l, 8, 18, 1);
	l.draw_text(44, 6, "thicket", true);
	l.draw_text(300, 6, "♥ ♥ ♡", true);
	l.draw_text(348, 6, "4d", true);
	soft_rule(l, 6, 24, 388);

	struct T { const char* who; const char* what; const char* when; bool mine; };
	const T tape[] = {
		{"mara", "are you coming down to the river", "9:14", false},
		{"you",  "on my way, ten minutes",           "9:15", true},
		{"mara", "bring the small lamp",             "9:20", false},
		{"sam",  "found the thing you left",         "8:02", false},
		{"you",  "keep it, i will come by",          "8:04", true},
	};
	const int SEL = 2;

	int y = 36;
	for (int i = 0; i < 5; ++i) {
		const bool sel = (i == SEL);
		const int x = tape[i].mine ? 60 : 14;
		if (sel) band(l, 8, y - 4, 340, 24);
		l.draw_text((uint16_t)x, (uint16_t)y, tape[i].who, !sel);
		l.draw_text((uint16_t)(x + 44), (uint16_t)y, tape[i].what, !sel);
		l.draw_text(300, (uint16_t)y, tape[i].when, !sel);
		if (tape[i].mine) l.draw_text(282, (uint16_t)y, "✓", !sel);
		y += 30;
	}

	rail(l, 366, 38, 148, 5, SEL);
	soft_rule(l, 6, 196, 388);
	l.draw_text(14, 206, "press to reply", true);
	l.draw_text(280, 206, "hold for more", true);
	l.flush(); save(p, "c2a-tape");

	// friends, with the rail doubling as who is reachable
	l.fill_white();
	sprig(l, 8, 18, 1);
	l.draw_text(44, 6, "who is near", true);
	soft_rule(l, 6, 24, 388);

	struct F { const char* name; const char* how; int near; };
	const F fr[] = {
		{"mara",            "close by",            3},
		{"sam",             "a way off",           2},
		{"the orchard relay","passing messages",   3},
		{"base",            "through the orchard", 1},
		{"ilse",            "not heard today",     0},
	};
	const int FSEL = 0;
	y = 40;
	for (int i = 0; i < 5; ++i) {
		const bool sel = (i == FSEL);
		if (sel) band(l, 8, y - 4, 340, 26);
		l.draw_text(20, (uint16_t)y, fr[i].name, !sel);
		l.draw_text(180, (uint16_t)y, fr[i].how, !sel);
		// nearness as growth: more leaves, not more bars
		for (int n = 0; n < 3; ++n) {
			const int lx = 316 + n * 10, ly = y + 8;
			if (n < fr[i].near)
				for (int dy = -3; dy <= 3; ++dy)
					for (int dx = -2; dx <= 2; ++dx)
						if (dx * dx * 2 + dy * dy <= 9)
							l.set_pixel((uint16_t)(lx + dx), (uint16_t)(ly + dy), !sel);
			else
				l.set_pixel((uint16_t)lx, (uint16_t)ly, !sel);
		}
		y += 32;
	}
	rail(l, 366, 42, 150, 5, FSEL);
	soft_rule(l, 6, 200, 388);
	l.draw_text(30, 210, "press to write to mara", true);
	l.flush(); save(p, "c2b-friends");
}

int main() {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	printf("[proposals2] rendering\n");
	b2(panel, lcd);
	c2(panel, lcd);
	printf("[proposals2] done\n");
	return 0;
}
