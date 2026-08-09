// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Message view as a conversation: bubbles, speaker sides, and tails.
//
// Renders to screens/*.pbm. Build: pio run -e proposals3

#include "SharpLcd.h"
#include "VirtualPanel.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

static const int CW = 6;      // advance
static const int LH = 14;     // line pitch inside a bubble

// --- chrome ----------------------------------------------------------------

static void round_rect(SharpLcd& l, int x, int y, int w, int h, int r, bool fill) {
	if (fill) {
		for (int yy = 0; yy < h; ++yy) {
			int inset = 0;
			const int dy = (yy < r) ? (r - yy) : (yy >= h - r ? yy - (h - 1 - r) : 0);
			if (dy > 0) inset = r - (int)__builtin_sqrtf((float)(r * r - dy * dy));
			for (int xx = inset; xx < w - inset; ++xx)
				l.set_pixel((uint16_t)(x + xx), (uint16_t)(y + yy), true);
		}
		return;
	}
	for (int i = r; i < w - r; ++i) {
		l.set_pixel((uint16_t)(x + i), (uint16_t)y, true);
		l.set_pixel((uint16_t)(x + i), (uint16_t)(y + h - 1), true);
	}
	for (int i = r; i < h - r; ++i) {
		l.set_pixel((uint16_t)x, (uint16_t)(y + i), true);
		l.set_pixel((uint16_t)(x + w - 1), (uint16_t)(y + i), true);
	}
	for (int a = 0; a <= r; ++a) {
		const int b = r - (int)__builtin_sqrtf((float)(r * r - (r - a) * (r - a)));
		l.set_pixel((uint16_t)(x + b), (uint16_t)(y + a), true);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + a), true);
		l.set_pixel((uint16_t)(x + b), (uint16_t)(y + h - 1 - a), true);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + h - 1 - a), true);
	}
}

// Wrap to a column count, on spaces.
static std::vector<std::string> wrap(const char* text, size_t cols) {
	std::vector<std::string> out;
	std::string line, word;
	for (const char* p = text; ; ++p) {
		if (*p && *p != ' ') { word += *p; continue; }
		if (!line.empty() && line.size() + 1 + word.size() > cols) {
			out.push_back(line); line = word;
		} else {
			if (!line.empty()) line += ' ';
			line += word;
		}
		word.clear();
		if (!*p) break;
	}
	if (!line.empty()) out.push_back(line);
	return out;
}

// One bubble. `mine` puts it right, filled, with the text knocked out.
static int bubble_height(const char* text) {
	auto lines = wrap(text, 32);
	return (int)lines.size() * LH + 14;
}

static int bubble(SharpLcd& l, int y, const char* text, bool mine,
                  const char* when, bool delivered) {
	const size_t COLS = 32;
	auto lines = wrap(text, COLS);
	size_t widest = 0;
	for (auto& s : lines) if (s.size() > widest) widest = s.size();

	const int pad = 7;
	const int bw = (int)widest * CW + pad * 2;
	const int bh = (int)lines.size() * LH + 14;   // 7px above and below
	const int bx = mine ? (394 - bw) : 6;

	round_rect(l, bx, y, bw, bh, 6, mine);
	// Tail: a wedge growing out of the lower corner on the speaker's side. It
	// has to overlap the body by a pixel or it reads as a detached mark.
	{
		const int ty0 = y + bh - 9;
		for (int i = 0; i < 6; ++i) {
			const int h2 = 6 - i;
			for (int k = 0; k < h2; ++k) {
				const int px = mine ? (bx + bw - 1 + i) : (bx - i);
				l.set_pixel((uint16_t)px, (uint16_t)(ty0 + k), true);
			}
		}
	}

	int ty = y + 6;
	for (auto& s : lines) {
		l.draw_text((uint16_t)(bx + pad), (uint16_t)ty, s.c_str(), !mine);
		ty += LH;
	}

	// time sits outside the bubble, small and quiet
	const int tw = (int)strlen(when) * CW;
	if (mine) {
		l.draw_text((uint16_t)(bx - tw - 12), (uint16_t)(y + bh - 15), when, true);
		if (delivered) l.draw_text((uint16_t)(bx - 10), (uint16_t)(y + bh - 15), "✓", true);
	} else {
		l.draw_text((uint16_t)(bx + bw + 8), (uint16_t)(y + bh - 15), when, true);
	}
	return y + bh + 10;
}

static void save(VirtualPanel& p, const char* name) {
	char path[128];
	snprintf(path, sizeof(path), "screens/%s.pbm", name);
	p.write_pbm(path, 2);
	printf("  %s\n", path);
}

// --- screens ---------------------------------------------------------------

static void header(SharpLcd& l, const char* who, const char* state) {
	round_rect(l, 4, 3, 392, 20, 6, true);
	l.draw_text(12, 6, who, false);
	const int sw = (int)strlen(state) * CW;
	l.draw_text((uint16_t)(388 - sw), 6, state, false);
}

static void composer(SharpLcd& l, const char* draft) {
	round_rect(l, 4, 208, 392, 28, 8, false);
	l.draw_text(14, 215, "→", true);
	l.draw_text(30, 215, draft, true);
	const int cx = 30 + (int)strlen(draft) * CW;
	l.fill_rect((uint16_t)cx, 214, 6, 13, true);
}

struct Line { const char* text; bool mine; const char* when; bool tick; };

// Newest at the bottom, resting on the composer, the way a phone does it.
static void thread(SharpLcd& l, const Line* rows, int n) {
	const int BOTTOM = 202;
	int total = 0;
	for (int i = 0; i < n; ++i) total += bubble_height(rows[i].text) + 10;
	int y = BOTTOM - total;
	if (y < 30) y = 30;
	for (int i = 0; i < n; ++i)
		y = bubble(l, y, rows[i].text, rows[i].mine, rows[i].when, rows[i].tick);
}

static void conversation(VirtualPanel& p, SharpLcd& l) {
	const Line a[] = {
		{"are you coming down to the river", false, "9:14", false},
		{"on my way, ten minutes",           true,  "9:15", true},
		{"bring the small lamp",             false, "9:20", false},
	};
	l.fill_white();
	header(l, "mara", "close by");
	thread(l, a, 3);
	composer(l, "yes, in my bag");
	l.flush(); save(p, "c3a-bubbles");

	const Line b[] = {
		{"found the thing you left by the gate", false, "8:02", false},
		{"keep it, i will come by tomorrow",     true,  "8:04", true},
		{"walking back now",                     true,  "sending", false},
	};
	l.fill_white();
	header(l, "sam", "a way off");
	thread(l, b, 3);
	composer(l, "");
	l.flush(); save(p, "c3b-sending");
}

int main() {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	printf("[proposals3] rendering\n");
	conversation(panel, lcd);
	printf("[proposals3] done\n");
	return 0;
}
