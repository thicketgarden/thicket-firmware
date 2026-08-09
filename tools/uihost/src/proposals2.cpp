// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wheel-first screen layouts. Every screen is a list with exactly one
// selection: rotating the wheel moves it, the axial press commits.
//
// Renders to screens/*.pbm. Build: pio run -e proposals2

#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "ui_marks.h"

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

static void save(VirtualPanel& p, const char* name) {
	char path[128];
	snprintf(path, sizeof(path), "screens/%s.pbm", name);
	p.write_pbm(path, 2);
	printf("  %s\n", path);
}

// ===========================================================================
// Framed list: heavy chrome, one selection, press to enter.
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

	// Quick replies: a send path that needs no keyboard at all.
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
// Minimal frame: a continuous ribbon of messages rather than boxed rows.
// ===========================================================================

static void c2_friends(VirtualPanel& p, SharpLcd& l);

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

	c2_friends(p, l);
}

// ---------------------------------------------------------------------------
// Peer list. Reachability is shown as the size of each peer's mark and stated
// in words beside it, rather than as a signal-strength bar.
// ---------------------------------------------------------------------------

static void c2_friends(VirtualPanel& p, SharpLcd& l) {
	struct F {
		const char* name;
		const char* how;
		const MarkArt* art;
		bool faded;     // not heard from: present, but not filled in
		bool waiting;   // something of theirs is here for you
	};
	const F fr[] = {
		{"mara",             "close by",            &MARK_L, false, true },
		{"sam",              "a way off",           &MARK_M, false, false},
		{"the orchard relay","passing messages",    &MARK_L, false, false},
		{"base",             "through the orchard", &MARK_S, false, false},
		{"ilse",             "not heard today",     &MARK_S, true,  false},
	};
	const int N = 5, FSEL = 0;

	const int TOP = 30, PITCH = 34, SLOT_X = 14, SLOT_W = 24;
	const int RAIL_X = 366;

	l.fill_white();
	sprig(l, 8, 18, 1);
	sprig(l, 392, 18, -1);   // mirrored, so the header is grown at both ends
	l.draw_text(44, 6, "who is near", true);
	soft_rule(l, 6, 24, 388);

	for (int i = 0; i < N; ++i) {
		const bool sel = (i == FSEL);
		const int row = TOP + PITCH * i;
		const int ground = row + 25;
		const bool ink = !sel;          // knocked out of the band when selected

		if (sel) band(l, 8, row - 1, 344, 32);

		const MarkArt& m = *fr[i].art;
		const int mx = SLOT_X + (SLOT_W - m.w) / 2;
		draw_mark(l, m, mx, ground, ink, fr[i].faded);

		// the ground they stand on, so they are in a place rather than floating.
		// It follows the mark's own width - a wide ground under a small mark
		// reads as a shelf the thing is sitting on rather than ground.
		for (int k = 0; k < m.w + 6; k += 2)
			l.set_pixel((uint16_t)(mx - 3 + k), (uint16_t)ground, ink);

		// the spore drifts off the edge of the cap rather than sitting centred
		// above it like a balloon, and it stays inside the selection band
		if (fr[i].waiting)
			draw_spore(l, mx + m.w - 3, ground - m.h - 2, ink);

		// names rest on the same ground the marks stand on, so a row is one
		// thing rather than a picture with a caption beside it
		const int ty_text = ground - 13;
		l.draw_text(52, (uint16_t)ty_text, fr[i].name, ink);
		l.draw_text(196, (uint16_t)ty_text, fr[i].how, ink);

		// rail tick beside the row it belongs to, so the stem reads as position
		const int ty = row + 14;
		if (sel) {
			for (int dy = -4; dy <= 4; ++dy)
				for (int dx = -4; dx <= 4; ++dx)
					if (dx * dx + dy * dy <= 16)
						l.set_pixel((uint16_t)(RAIL_X + dx), (uint16_t)(ty + dy), true);
		} else {
			const int dir = (i & 1) ? 1 : -1;
			for (int k = 2; k <= 5; ++k)
				l.set_pixel((uint16_t)(RAIL_X + k * dir), (uint16_t)ty, true);
		}
	}

	for (int i = 0; i < TOP + PITCH * (N - 1) + 14 - 30; ++i)
		if ((i & 3) != 3) l.set_pixel((uint16_t)RAIL_X, (uint16_t)(30 + i), true);

	soft_rule(l, 6, 206, 388);
	// the footer offers what pressing actually does here: if they have left
	// something, reading it comes before writing back
	char foot[64];
	if (fr[FSEL].waiting)
		snprintf(foot, sizeof(foot), "press to read what %s sent", fr[FSEL].name);
	else
		snprintf(foot, sizeof(foot), "press to write to %s", fr[FSEL].name);
	l.draw_text(30, 212, foot, true);
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
