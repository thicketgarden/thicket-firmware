// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// D: cards. Conventional list/detail patterns — status bar, cards, scrollbar,
// badges, bubbles — drawn for a 1-bit panel. Outline vs fill carries selection
// because it is the only strong contrast available.

#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "ui_marks.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

static const int CW = 6;            // glyph advance
static const int MARGIN = 8;
static const int CARD_X = MARGIN;
static const int CARD_W = 364;
static const int TRACK_X = 380;
static const int STATUS_H = 18;
static const int BODY_TOP = 24;
static const int HINT_Y = 208;

// --- primitives ------------------------------------------------------------

static int corner_inset(int yy, int h, int r) {
	if (r <= 0) return 0;
	const int dy = (yy < r) ? (r - yy) : (yy >= h - r ? yy - (h - 1 - r) : 0);
	if (dy <= 0) return 0;
	return r - (int)__builtin_sqrtf((float)(r * r - dy * dy));
}

// Separate top and bottom radii so a card's title strip can be filled with its
// top corners rounded and its bottom square.
static void rrect_fill(SharpLcd& l, int x, int y, int w, int h,
                       int rt, int rb, bool ink) {
	for (int yy = 0; yy < h; ++yy) {
		const int r = (yy < h / 2) ? rt : rb;
		const int ins = corner_inset(yy, h, r);
		for (int xx = ins; xx < w - ins; ++xx)
			l.set_pixel((uint16_t)(x + xx), (uint16_t)(y + yy), ink);
	}
}

static void rrect_line(SharpLcd& l, int x, int y, int w, int h, int r, bool ink) {
	for (int i = r; i < w - r; ++i) {
		l.set_pixel((uint16_t)(x + i), (uint16_t)y, ink);
		l.set_pixel((uint16_t)(x + i), (uint16_t)(y + h - 1), ink);
	}
	for (int i = r; i < h - r; ++i) {
		l.set_pixel((uint16_t)x, (uint16_t)(y + i), ink);
		l.set_pixel((uint16_t)(x + w - 1), (uint16_t)(y + i), ink);
	}
	for (int a = 0; a <= r; ++a) {
		const int b = r - (int)__builtin_sqrtf((float)(r * r - (r - a) * (r - a)));
		l.set_pixel((uint16_t)(x + b),         (uint16_t)(y + a),         ink);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + a),         ink);
		l.set_pixel((uint16_t)(x + b),         (uint16_t)(y + h - 1 - a), ink);
		l.set_pixel((uint16_t)(x + w - 1 - b), (uint16_t)(y + h - 1 - a), ink);
	}
}

static int tw(const char* s) { return (int)strlen(s) * CW; }

static void text_right(SharpLcd& l, int right, int y, const char* s, bool ink) {
	l.draw_text((uint16_t)(right - tw(s)), (uint16_t)y, s, ink);
}

// Unread count. Filled pill, count knocked out — the familiar badge.
static void badge(SharpLcd& l, int right, int y, int n, bool ink) {
	char s[8];
	snprintf(s, sizeof(s), "%d", n);
	const int w = tw(s) + 12, h = 15;
	rrect_fill(l, right - w, y, w, h, 7, 7, ink);
	l.draw_text((uint16_t)(right - w + 6), (uint16_t)(y + 1), s, !ink);
}

static void battery(SharpLcd& l, int x, int y, int pct) {
	rrect_line(l, x, y, 22, 11, 2, true);
	for (int i = 3; i <= 7; ++i) l.set_pixel((uint16_t)(x + 22), (uint16_t)(y + i), true);
	const int fw = (18 * pct) / 100;
	if (fw > 0) l.fill_rect((uint16_t)(x + 2), (uint16_t)(y + 3), (uint16_t)fw, 5, true);
}

static void scrollbar(SharpLcd& l, int y, int h, int total, int shown, int first) {
	rrect_line(l, TRACK_X, y, 6, h, 2, true);
	if (total <= shown) return;
	int th = h * shown / total;
	if (th < 14) th = 14;
	const int ty = y + (h - th) * first / (total - shown);
	rrect_fill(l, TRACK_X + 2, ty + 2, 2, th - 4, 1, 1, true);
}

static void status_bar(SharpLcd& l, const char* title, const char* clock, int pct) {
	sprig(l, 6, 12, 1);   // leaves hang 4px below the stem; keep them off the rule
	l.draw_text(30, 2, title, true);
	const int bx = 400 - MARGIN - 24;
	battery(l, bx, 3, pct);
	text_right(l, bx - 10, 2, clock, true);
	l.draw_hline(0, STATUS_H, LCD_WIDTH, true);
}

static void hint_bar(SharpLcd& l, const char* left, const char* right, int y) {
	l.draw_hline(0, (uint16_t)y, LCD_WIDTH, true);
	l.draw_text(MARGIN, (uint16_t)(y + 6), left, true);
	if (right) text_right(l, 400 - MARGIN, y + 6, right, true);
}

// Card. Selected gets a second border and a filled title strip.
static void card(SharpLcd& l, int y, int h, bool sel, int head_h) {
	rrect_line(l, CARD_X, y, CARD_W, h, 6, true);
	if (!sel) {
		if (head_h > 0) l.draw_hline((uint16_t)(CARD_X + 1), (uint16_t)(y + head_h),
		                             (uint16_t)(CARD_W - 2), true);
		return;
	}
	rrect_line(l, CARD_X + 1, y + 1, CARD_W - 2, h - 2, 5, true);
	if (head_h > 0) rrect_fill(l, CARD_X + 2, y + 2, CARD_W - 4, head_h - 2, 4, 0, true);
}

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

static void save(VirtualPanel& p, const char* name) {
	char path[128];
	snprintf(path, sizeof(path), "screens/%s.pbm", name);
	p.write_pbm(path, 2);
	printf("  %s\n", path);
}

// --- screens ---------------------------------------------------------------

static void d_messages(VirtualPanel& p, SharpLcd& l) {
	struct Conv {
		const char* who; const char* last; const char* route;
		const char* when; int unread;
	};
	const Conv rows[] = {
		{"mara", "Are you coming down to the river?", "direct",             "9:14", 2},
		{"sam",  "Found the thing you left by the gate", "via orchard relay", "1h",  0},
		{"base", "Weather turning tonight",           "direct",             "3h",   0},
	};
	const int SEL = 0, H = 56, GAP = 5;

	l.fill_white();
	status_bar(l, "messages", "9:22", 72);

	for (int i = 0; i < 3; ++i) {
		const int y = BODY_TOP + i * (H + GAP);
		const bool sel = (i == SEL);
		card(l, y, H, sel, 20);
		const bool head_ink = !sel;

		l.draw_text(CARD_X + 10, (uint16_t)(y + 4), rows[i].who, head_ink);
		int right = CARD_X + CARD_W - 10;
		if (rows[i].unread) {
			badge(l, right, y + 3, rows[i].unread, head_ink);
			right -= tw("2") + 12 + 8;
		}
		text_right(l, right, y + 4, rows[i].when, head_ink);

		l.draw_text(CARD_X + 10, (uint16_t)(y + 26), rows[i].last, true);
		l.draw_text(CARD_X + 10, (uint16_t)(y + 41), rows[i].route, true);
	}

	scrollbar(l, BODY_TOP, 3 * (H + GAP) - GAP, 6, 3, 0);
	hint_bar(l, "press to open", "turn to move", HINT_Y);
	l.flush(); save(p, "d1-messages");
}

static int bubble(SharpLcd& l, int y, const char* text, bool mine, const char* when) {
	const size_t COLS = 34;
	const int LH = 14, PAD = 7;
	auto lines = wrap(text, COLS);
	size_t widest = 0;
	for (auto& s : lines) if (s.size() > widest) widest = s.size();

	const int bw = (int)widest * CW + PAD * 2;
	const int bh = (int)lines.size() * LH + 12;
	const int bx = mine ? (392 - bw) : MARGIN;

	if (mine) rrect_fill(l, bx, y, bw, bh, 6, 6, true);
	else      rrect_line(l, bx, y, bw, bh, 6, true);

	int ty = y + 5;
	for (auto& s : lines) {
		l.draw_text((uint16_t)(bx + PAD), (uint16_t)ty, s.c_str(), !mine);
		ty += LH;
	}
	if (mine) text_right(l, bx - 8, y + bh - 15, when, true);
	else      l.draw_text((uint16_t)(bx + bw + 8), (uint16_t)(y + bh - 15), when, true);
	return y + bh + 8;
}

static int bubble_h(const char* text) {
	return (int)wrap(text, 34).size() * 14 + 12;
}

static void d_conversation(VirtualPanel& p, SharpLcd& l) {
	struct Msg { const char* text; bool mine; const char* when; };
	const Msg thread[] = {
		{"Are you coming down to the river?", false, "9:14"},
		{"On my way, ten minutes",            true,  "9:15"},
		{"Bring the small lamp",              false, "9:20"},
	};
	const int N = 3;

	l.fill_white();
	status_bar(l, "mara", "9:22", 72);
	l.draw_text(MARGIN, 24, "direct", true);
	text_right(l, 392, 24, "delivered 9:15", true);

	// bottom-anchored: the thread rests on the composer and grows upward
	int total = -8;
	for (int i = 0; i < N; ++i) total += bubble_h(thread[i].text) + 8;
	int y = 174 - total;
	if (y < 42) y = 42;
	for (int i = 0; i < N; ++i)
		y = bubble(l, y, thread[i].text, thread[i].mine, thread[i].when);

	rrect_line(l, CARD_X, 186, CARD_W + 16, 26, 6, true);
	l.draw_text(CARD_X + 10, 192, "Yes, in my bag", true);
	l.fill_rect((uint16_t)(CARD_X + 12 + tw("Yes, in my bag")), 191, 6, 13, true);
	hint_bar(l, "press to send", "hold to go back", 216);
	l.flush(); save(p, "d2-conversation");
}

static void d_people(VirtualPanel& p, SharpLcd& l) {
	struct Person { const char* name; const char* route; const char* seen; };
	const Person rows[] = {
		{"mara",          "direct",             "last seen just now"},
		{"sam",           "via orchard relay",  "last seen 1h ago"},
		{"base",          "direct",             "last seen 3h ago"},
		{"ilse",          "not reachable",      "last seen yesterday"},
	};
	const int SEL = 0, H = 42, GAP = 4;

	l.fill_white();
	status_bar(l, "people", "9:22", 72);

	for (int i = 0; i < 4; ++i) {
		const int y = BODY_TOP + i * (H + GAP);
		const bool sel = (i == SEL);
		card(l, y, H, sel, 20);
		const bool head_ink = !sel;
		l.draw_text(CARD_X + 10, (uint16_t)(y + 4), rows[i].name, head_ink);
		text_right(l, CARD_X + CARD_W - 10, y + 4, rows[i].route, head_ink);
		l.draw_text(CARD_X + 10, (uint16_t)(y + 25), rows[i].seen, true);
	}

	scrollbar(l, BODY_TOP, 4 * (H + GAP) - GAP, 7, 4, 0);
	hint_bar(l, "press to write", "turn to move", HINT_Y);
	l.flush(); save(p, "d3-people");
}

static void d_compose(VirtualPanel& p, SharpLcd& l) {
	const char* quick[] = {"On my way", "Ten minutes", "Not tonight",
	                       "Yes", "Call me", "Type something…"};
	const int SEL = 1, H = 26, GAP = 3;

	l.fill_white();
	status_bar(l, "to mara", "9:22", 72);

	for (int i = 0; i < 6; ++i) {
		const int y = BODY_TOP + i * (H + GAP);
		const bool sel = (i == SEL);
		if (sel) rrect_fill(l, CARD_X, y, CARD_W, H, 6, 6, true);
		else     rrect_line(l, CARD_X, y, CARD_W, H, 6, true);
		l.draw_text(CARD_X + 12, (uint16_t)(y + 6), quick[i], !sel);
	}

	scrollbar(l, BODY_TOP, 6 * (H + GAP) - GAP, 8, 6, 0);
	hint_bar(l, "press to send", "turn to move", HINT_Y);
	l.flush(); save(p, "d4-compose");
}

// The panel holds its image with no redraw, so the idle screen is static by
// design: every frame costs a 12,000-byte transfer. Nothing here animates.
static void d_rest(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	status_bar(l, "thicket", "9:22", 72);

	card(l, BODY_TOP, 74, true, 20);
	l.draw_text(CARD_X + 10, BODY_TOP + 4, "2 messages waiting", false);
	l.draw_text(CARD_X + 10, BODY_TOP + 28, "mara", true);
	text_right(l, CARD_X + CARD_W - 10, BODY_TOP + 28, "9:14", true);
	l.draw_text(CARD_X + 10, BODY_TOP + 46, "sam", true);
	text_right(l, CARD_X + CARD_W - 10, BODY_TOP + 46, "8:02", true);

	card(l, 106, 44, false, 0);
	l.draw_text(CARD_X + 10, 113, "4 people reachable", true);
	text_right(l, CARD_X + CARD_W - 10, 113, "1 relay", true);
	l.draw_text(CARD_X + 10, 129, "orchard relay is passing messages", true);

	card(l, 156, 44, false, 0);
	l.draw_text(CARD_X + 10, 163, "listening", true);
	text_right(l, CARD_X + CARD_W - 10, 163, "3 days of battery", true);
	l.draw_text(CARD_X + 10, 179, "last heard from mara 8 minutes ago", true);

	hint_bar(l, "press to open messages", NULL, HINT_Y);
	l.flush(); save(p, "d5-rest");
}

static void d_first_run(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	status_bar(l, "thicket", "9:22", 100);

	card(l, 54, 108, true, 22);
	l.draw_text(CARD_X + 10, 58, "No one added yet", false);
	l.draw_text(CARD_X + 10, 88, "Hold this next to another thicket", true);
	l.draw_text(CARD_X + 10, 104, "and press the wheel on both. They", true);
	l.draw_text(CARD_X + 10, 120, "will remember each other after that.", true);
	l.draw_text(CARD_X + 10, 142, "Nothing is sent until you add someone.", true);

	hint_bar(l, "press to add someone", NULL, HINT_Y);
	l.flush(); save(p, "d6-first-run");
}

static void d_queued(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	status_bar(l, "messages", "9:22", 41);

	card(l, 54, 96, true, 22);
	l.draw_text(CARD_X + 10, 58, "Nothing in range", false);
	l.draw_text(CARD_X + 10, 88, "3 messages are waiting to go out.", true);
	l.draw_text(CARD_X + 10, 106, "They will send on their own as soon", true);
	l.draw_text(CARD_X + 10, 122, "as anything comes within reach.", true);

	card(l, 162, 38, false, 0);
	l.draw_text(CARD_X + 10, 172, "Last in range", true);
	text_right(l, CARD_X + CARD_W - 10, 172, "40 minutes ago", true);

	hint_bar(l, "press to see them", "turn to move", HINT_Y);
	l.flush(); save(p, "d7-queued");
}

int main() {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	printf("[proposals4] rendering\n");
	d_messages(panel, lcd);
	d_conversation(panel, lcd);
	d_people(panel, lcd);
	d_compose(panel, lcd);
	d_rest(panel, lcd);
	d_first_run(panel, lcd);
	d_queued(panel, lcd);
	printf("[proposals4] done\n");
	return 0;
}
