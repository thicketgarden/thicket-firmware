// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The refined direction: c2's header mark, texting-style bubbles, one system.

#include "SharpLcd.h"
#include "VirtualPanel.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

static const int CW = 6;
static const int LH = 14;
static const int M  = 10;          // page margin

// --- primitives ------------------------------------------------------------

static void rr(SharpLcd& l, int x, int y, int w, int h, int r, bool fill) {
	if (fill) {
		for (int yy = 0; yy < h; ++yy) {
			const int dy = (yy < r) ? (r - yy) : (yy >= h - r ? yy - (h - 1 - r) : 0);
			const int in = dy > 0 ? r - (int)__builtin_sqrtf((float)(r*r - dy*dy)) : 0;
			for (int xx = in; xx < w - in; ++xx)
				l.set_pixel((uint16_t)(x+xx), (uint16_t)(y+yy), true);
		}
		return;
	}
	for (int i = r; i < w - r; ++i) {
		l.set_pixel((uint16_t)(x+i), (uint16_t)y, true);
		l.set_pixel((uint16_t)(x+i), (uint16_t)(y+h-1), true);
	}
	for (int i = r; i < h - r; ++i) {
		l.set_pixel((uint16_t)x, (uint16_t)(y+i), true);
		l.set_pixel((uint16_t)(x+w-1), (uint16_t)(y+i), true);
	}
	for (int a = 0; a <= r; ++a) {
		const int b = r - (int)__builtin_sqrtf((float)(r*r - (r-a)*(r-a)));
		l.set_pixel((uint16_t)(x+b),       (uint16_t)(y+a),       true);
		l.set_pixel((uint16_t)(x+w-1-b),   (uint16_t)(y+a),       true);
		l.set_pixel((uint16_t)(x+b),       (uint16_t)(y+h-1-a),   true);
		l.set_pixel((uint16_t)(x+w-1-b),   (uint16_t)(y+h-1-a),   true);
	}
}

// A small leaf. Drawn as pixel art: at seven across, a solved ellipse comes out
// as a bar, which is the signal-strength language this is meant to avoid.
static const uint8_t LEAF[5] = {
	0b0011100,
	0b0111110,
	0b1111111,
	0b0111110,
	0b0011000,
};

static void leaf(SharpLcd& l, int x, int y, bool on) {
	for (int r = 0; r < 5; ++r)
		for (int c = 0; c < 7; ++c)
			if (LEAF[r] & (0x40 >> c))
				l.set_pixel((uint16_t)(x + c), (uint16_t)(y + r), on);
}

// The mark: a fruiting body. Hand-drawn rather than solved, because at 24px a
// curve has to be placed pixel by pixel to read. The design language calls the
// device a fruiting body, so the mark is the thing itself.
static const uint32_t MARK[20] = {
	0x007E00, 0x01FF80, 0x07FFE0, 0x0FFFF0,
	0x1FFFF8, 0x3FFFFC, 0x7FFFFE, 0x7FFFFE,
	0x3FFFFC, 0x01FF80, 0x007E00, 0x007E00,
	0x007E00, 0x007E00, 0x007E00, 0x00FF00,
	0x00FF00, 0x01FF80, 0x03FFC0, 0x07FFE0,
};

static void mark(SharpLcd& l, int x, int y, int scale) {
	for (int row = 0; row < 20; ++row) {
		for (int col = 0; col < 24; ++col) {
			if (!(MARK[row] & (0x800000u >> col))) continue;
			// gills: a lighter band where the cap meets the stem
			const bool gill = (row == 8 && ((col + row) & 1));
			for (int sy = 0; sy < scale; ++sy)
				for (int sx = 0; sx < scale; ++sx)
					l.set_pixel((uint16_t)(x + col * scale + sx),
					            (uint16_t)(y + row * scale + sy), !gill);
		}
	}
}

static void hairline(SharpLcd& l, int x, int y, int w) {
	// Fades toward the middle: a seam rather than a cut.
	for (int i = 0; i < w; ++i) {
		const float t = (float)i / (float)(w - 1);
		const float e = (t < 0.5f) ? t * 2.f : (1.f - t) * 2.f;
		const int step = 1 + (int)(e * 4.f);
		if (i % step == 0) l.set_pixel((uint16_t)(x+i), (uint16_t)y, true);
	}
}

static std::vector<std::string> wrap(const char* s, size_t cols) {
	std::vector<std::string> out; std::string line, w;
	for (const char* p = s;; ++p) {
		if (*p && *p != ' ') { w += *p; continue; }
		if (!line.empty() && line.size() + 1 + w.size() > cols) { out.push_back(line); line = w; }
		else { if (!line.empty()) line += ' '; line += w; }
		w.clear();
		if (!*p) break;
	}
	if (!line.empty()) out.push_back(line);
	return out;
}

// --- chrome ----------------------------------------------------------------

// The header the design settled on: mark, wordmark, and what the screen is.
static void head(SharpLcd& l, const char* right) {
	mark(l, M, 5, 1);
	l.draw_text(M + 32, 8, "thicket", true);
	if (right && *right) {
		const int w = (int)strlen(right) * CW;
		l.draw_text((uint16_t)(400 - M - w), 8, right, true);
	}
	hairline(l, M, 28, 400 - 2 * M);
}

static void foot(SharpLcd& l, const char* left, const char* right) {
	hairline(l, M, 212, 400 - 2 * M);
	if (left)  l.draw_text(M, 220, left, true);
	if (right) {
		const int w = (int)strlen(right) * CW;
		l.draw_text((uint16_t)(400 - M - w), 220, right, true);
	}
}

static int bubble_h(const char* t) { return (int)wrap(t, 30).size() * LH + 14; }

static int bubble(SharpLcd& l, int y, const char* text, bool mine,
                  const char* when, const char* state) {
	auto lines = wrap(text, 30);
	size_t wide = 0; for (auto& s : lines) if (s.size() > wide) wide = s.size();
	const int pad = 8;
	const int bw = (int)wide * CW + pad * 2;
	const int bh = (int)lines.size() * LH + 14;
	const int bx = mine ? (400 - M - bw) : M;

	rr(l, bx, y, bw, bh, 7, mine);
	// tail, overlapping the body so it grows out of the corner
	for (int i = 0; i < 7; ++i) {
		const int hh = 7 - i;
		for (int k = 0; k < hh; ++k) {
			const int px = mine ? (bx + bw - 2 + i) : (bx + 1 - i);
			l.set_pixel((uint16_t)px, (uint16_t)(y + bh - 10 + k), true);
		}
	}

	int ty = y + 6;
	for (auto& s : lines) { l.draw_text((uint16_t)(bx+pad), (uint16_t)ty, s.c_str(), !mine); ty += LH; }

	char meta[48];
	if (mine) snprintf(meta, sizeof(meta), "%s %s", when, state ? state : "");
	else      snprintf(meta, sizeof(meta), "%s", when);
	const int mw = (int)strlen(meta) * CW;
	if (mine) l.draw_text((uint16_t)(bx - mw - 10), (uint16_t)(y + bh - 16), meta, true);
	else      l.draw_text((uint16_t)(bx + bw + 10), (uint16_t)(y + bh - 16), meta, true);
	return y + bh + 10;
}

static void save(VirtualPanel& p, const char* n) {
	char path[128]; snprintf(path, sizeof(path), "screens/%s.pbm", n);
	p.write_pbm(path, 2); printf("  %s\n", n);
}

// --- screens ---------------------------------------------------------------

static void resting(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	head(l, "4 days");

	// The same mark, large. The device at rest is the mark itself.
	mark(l, 152, 56, 4);   // 20 rows x4 = 80px tall, so the base lands at 136

	l.draw_text(124, 154, "everything is quiet", true);
	l.draw_text(136, 176, "3 friends near", true);
	foot(l, "turn to look around", "♥♥♡");
	l.flush(); save(p, "r1-resting");
}

static void conversation(VirtualPanel& p, SharpLcd& l) {
	struct L { const char* t; bool mine; const char* when; const char* st; };
	const L rows[] = {
		{"are you coming down to the river", false, "9:14", nullptr},
		{"on my way, ten minutes",           true,  "9:15", "✓"},
		{"bring the small lamp",             false, "9:20", nullptr},
	};
	l.fill_white();
	head(l, "mara · close by");
	int total = 0; for (auto& r : rows) total += bubble_h(r.t) + 10;
	int y = 204 - total; if (y < 38) y = 38;
	for (auto& r : rows) y = bubble(l, y, r.t, r.mine, r.when, r.st);
	foot(l, "press to reply", "turn for others");
	l.flush(); save(p, "r2-conversation");
}

static void people(VirtualPanel& p, SharpLcd& l) {
	struct F { const char* n; const char* s; int near; bool unread; };
	const F fr[] = {
		{"mara",             "close by",           3, true},
		{"sam",              "a way off",          2, true},
		{"the orchard relay","passing messages",   3, false},
		{"base",             "through the orchard",1, false},
	};
	const int SEL = 0;
	l.fill_white();
	head(l, "2 new");
	int y = 44;
	for (int i = 0; i < 4; ++i) {
		const bool sel = (i == SEL);
		if (sel) rr(l, M - 4, y - 6, 400 - 2*M + 8, 30, 8, true);
		l.draw_text(M + 4, (uint16_t)y, fr[i].n, !sel);
		l.draw_text(200,   (uint16_t)y, fr[i].s, !sel);
		if (fr[i].unread) l.draw_text((uint16_t)(400 - M - 44), (uint16_t)y, "●", !sel);
		// nearness as leaves, on a short stem
		for (int k = 0; k < fr[i].near; ++k)
			leaf(l, 400 - M - 30 + k * 9, y + 3, !sel);
		y += 40;
	}
	foot(l, "press to open", "♥♥♡");
	l.flush(); save(p, "r3-people");
}

int main() {
	VirtualPanel panel; SharpLcd lcd(panel, fb);
	printf("[refined]\n");
	resting(panel, lcd);
	conversation(panel, lcd);
	people(panel, lcd);
	return 0;
}
