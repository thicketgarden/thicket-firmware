// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Card-based screen layouts: status bar, cards, scrollbar, badges, and
// conversation bubbles, drawn for a 1-bit panel. Selection is carried by
// outline versus fill, the only strong contrast the panel offers.
//
// Renders to screens/*.pbm. Build: pio run -e proposals4

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

// Unread count as a filled pill with the number knocked out.
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

struct Conv {
	const char* who; const char* last; const char* route;
	const char* when; int unread;
};
static const Conv CONVS[3] = {
	{"mara", "Are you coming down to the river?",    "direct",            "9:14", 2},
	{"sam",  "Found the thing you left by the gate", "via orchard relay", "1h",   0},
	{"base", "Weather turning tonight",              "direct",            "3h",   0},
};
static const int CONV_H = 58, CONV_GAP = 4;

// Draws the selected card: filled, with a highlight line inset from its edge.
// The panel is 1-bit, so an inset line is what gives the fill any depth.
static void select_card(SharpLcd& l, int y, int h, int inset = 4) {
	rrect_fill(l, CARD_X, y, CARD_W, h, 6, 6, true);
	rrect_line(l, CARD_X + inset, y + inset, CARD_W - 2 * inset, h - 2 * inset,
	           inset < 4 ? 3 : 4, false);
}

struct Ball;
static void dot_field(SharpLcd& l, int step);

// Ground shared by the screens that carry one message rather than a list, so
// they read as the same family as the idle screen.
static void hero_ground(SharpLcd& l) {
	dot_field(l, 16);
	l.fill_rect(0, 0, LCD_WIDTH, 20, false);
	l.fill_rect(0, 204, LCD_WIDTH, 36, false);
}

static void d_messages_frame(SharpLcd& l, int sel) {
	l.fill_white();
	status_bar(l, "messages", "9:22", 72);

	for (int i = 0; i < 3; ++i) {
		const int y = BODY_TOP + i * (CONV_H + CONV_GAP);
		const bool s = (i == sel);
		if (s) select_card(l, y, CONV_H);
		else   rrect_line(l, CARD_X, y, CARD_W, CONV_H, 6, true);
		const bool ink = !s;

		// Sender at 2x: it is the field the list is scanned by.
		l.draw_text_scaled((uint16_t)(CARD_X + 12), (uint16_t)(y + 6),
		                   CONVS[i].who, ink, 2);

		int right = CARD_X + CARD_W - 12;
		if (CONVS[i].unread) {
			badge(l, right, y + 11, CONVS[i].unread, ink);
			right -= tw("2") + 12 + 8;
		}
		text_right(l, right, y + 12, CONVS[i].when, ink);

		l.draw_text((uint16_t)(CARD_X + 12), (uint16_t)(y + 38), CONVS[i].last, ink);
		text_right(l, CARD_X + CARD_W - 12, y + 38, CONVS[i].route, ink);
	}

	scrollbar(l, BODY_TOP, 3 * (CONV_H + CONV_GAP) - CONV_GAP, 6, 3, 0);
	hint_bar(l, "press to open", "turn to move", HINT_Y);
}

static uint8_t compose_fb[LCD_FB_BYTES];

// Copies a composed frame across pixel by pixel, so a line is marked dirty
// only where it actually changed. Drawing in place would dirty every line it
// touched, including lines whose final contents are identical.
static void blit(SharpLcd& dst, SharpLcd& src) {
	for (uint16_t y = 0; y < LCD_HEIGHT; ++y)
		for (uint16_t x = 0; x < LCD_WIDTH; ++x)
			dst.set_pixel(x, y, src.get_pixel(x, y));
}

static void d_messages(VirtualPanel& p, SharpLcd& l) {
	VirtualPanel sink;
	SharpLcd c(sink, compose_fb);

	l.fill_white();
	d_messages_frame(c, 0);
	blit(l, c);
	p.reset_counters();
	l.flush();
	save(p, "d1-messages");

	// Cost of moving the selection one row, reported on stdout.
	d_messages_frame(c, 1);
	blit(l, c);
	p.reset_counters();
	l.flush();
	save(p, "d1-messages-next");
	printf("    one wheel detent: %u lines, %u bytes on the wire\n",
	       (unsigned)p.lines_written(), (unsigned)(2 + 52 * p.lines_written()));
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
		{"mara", "direct",            "last seen just now"},
		{"sam",  "via orchard relay", "last seen 1h ago"},
		{"base", "direct",            "last seen 3h ago"},
		{"ilse", "not reachable",     "last seen yesterday"},
	};
	// Four kept visible rather than three: a contact list is scanned for a
	// name you already know, so density beats size here.
	const int SEL = 0, H = 42, GAP = 3;

	l.fill_white();
	status_bar(l, "people", "9:22", 72);

	for (int i = 0; i < 4; ++i) {
		const int y = BODY_TOP + i * (H + GAP);
		const bool s = (i == SEL);
		if (s) select_card(l, y, H, 3);
		else   rrect_line(l, CARD_X, y, CARD_W, H, 6, true);
		const bool ink = !s;
		l.draw_text((uint16_t)(CARD_X + 12), (uint16_t)(y + 7), rows[i].name, ink);
		text_right(l, CARD_X + CARD_W - 12, y + 7, rows[i].route, ink);
		l.draw_text((uint16_t)(CARD_X + 12), (uint16_t)(y + 23), rows[i].seen, ink);
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
		if (sel) select_card(l, y, H, 3);
		else     rrect_line(l, CARD_X, y, CARD_W, H, 6, true);
		l.draw_text(CARD_X + 12, (uint16_t)(y + 6), quick[i], !sel);
	}

	scrollbar(l, BODY_TOP, 6 * (H + GAP) - GAP, 8, 6, 0);
	hint_bar(l, "press to send", "turn to move", HINT_Y);
	l.flush(); save(p, "d4-compose");
}

// --- the idle dashboard ----------------------------------------------------
//
// The panel holds its image without redraw, so an idle frame costs nothing
// until something moves. Motion here is therefore deliberate and slow: see
// rest_compose() for the per-frame transfer cost this trades against.

struct Ball { float x, y, r; };

static float field(const Ball* b, int n, float px, float py) {
	float s = 0.0f;
	for (int i = 0; i < n; ++i) {
		const float dx = px - b[i].x, dy = py - b[i].y;
		const float d2 = dx * dx + dy * dy;
		s += (b[i].r * b[i].r) / (d2 < 1.0f ? 1.0f : d2);
	}
	return s;
}

// Renders a metaball field as a filled or outlined shape.
//
// Generated rather than hand-drawn, which suits these shapes at ~170 px across.
// The small marks in ui_marks.h are hand-drawn for the opposite reason.
static void blob(SharpLcd& l, const Ball* b, int n, int x0, int y0, int x1, int y1,
                 bool fill, bool ink) {
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			if (field(b, n, (float)x, (float)y) < 1.0f) continue;
			if (fill) { l.set_pixel((uint16_t)x, (uint16_t)y, ink); continue; }
			const bool edge = field(b, n, (float)(x - 2), (float)y) < 1.0f ||
			                  field(b, n, (float)(x + 2), (float)y) < 1.0f ||
			                  field(b, n, (float)x, (float)(y - 2)) < 1.0f ||
			                  field(b, n, (float)x, (float)(y + 2)) < 1.0f;
			if (edge) l.set_pixel((uint16_t)x, (uint16_t)y, ink);
		}
	}
}

// Knocks a highlight line out of a filled blob, inset from its edge.
static void blob_inner_ring(SharpLcd& l, const Ball* b, int n,
                            int x0, int y0, int x1, int y1, int inset) {
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			if (field(b, n, (float)x, (float)y) < 1.0f) continue;
			const bool near_out =
				field(b, n, (float)(x - inset), (float)y) < 1.0f ||
				field(b, n, (float)(x + inset), (float)y) < 1.0f ||
				field(b, n, (float)x, (float)(y - inset)) < 1.0f ||
				field(b, n, (float)x, (float)(y + inset)) < 1.0f;
			const bool on_edge =
				field(b, n, (float)(x - inset / 2), (float)y) < 1.0f ||
				field(b, n, (float)(x + inset / 2), (float)y) < 1.0f ||
				field(b, n, (float)x, (float)(y - inset / 2)) < 1.0f ||
				field(b, n, (float)x, (float)(y + inset / 2)) < 1.0f;
			if (near_out && !on_edge) l.set_pixel((uint16_t)x, (uint16_t)y, false);
		}
	}
}

static void dot_field(SharpLcd& l, int step) {
	for (int y = 6; y < 240; y += step)
		for (int x = 6; x < 400; x += step)
			l.set_pixel((uint16_t)x, (uint16_t)y, true);
}

// Draws text over the dot field, clearing a box behind it first so no stray
// dot lands against a glyph.
static void field_text(SharpLcd& l, int x, int y, const char* s, int scale) {
	const int w = tw(s) * scale, h = 13 * scale;
	l.fill_rect((uint16_t)(x - 7), (uint16_t)(y - 2), (uint16_t)(w + 14),
	            (uint16_t)(h + 4), false);
	if (scale <= 1) l.draw_text((uint16_t)x, (uint16_t)y, s, true);
	else            l.draw_text_scaled((uint16_t)x, (uint16_t)y, s, true, (uint8_t)scale);
}

// Segmented capsule. Reads as a gauge without borrowing the battery icon.
static void gauge(SharpLcd& l, int x, int y, int w, int h, int filled, int cells) {
	rrect_line(l, x, y, w, h, h / 2, true);
	const int cw = (w - 8) / cells;
	for (int i = 0; i < filled && i < cells; ++i)
		l.fill_rect((uint16_t)(x + 4 + i * cw + 1), (uint16_t)(y + 3),
		            (uint16_t)(cw - 2), (uint16_t)(h - 6), true);
}

// The shell and the two count tiles, as they sit at rest. Each frame displaces
// them slightly; the phases differ so the masses do not pulse in unison.
static const Ball SHELL0[4] = {
	{78.0f,  74.0f,  40.0f},
	{108.0f, 120.0f, 52.0f},
	{76.0f,  164.0f, 40.0f},
	{140.0f, 112.0f, 34.0f},
};
static const Ball TILE1_0[3] = {
	{252.0f, 64.0f, 30.0f}, {302.0f, 64.0f, 32.0f}, {352.0f, 64.0f, 30.0f},
};
static const Ball TILE2_0[3] = {
	{252.0f, 162.0f, 30.0f}, {302.0f, 162.0f, 32.0f}, {352.0f, 162.0f, 30.0f},
};

static const int FRAMES = 6;

// Displacement returns to zero at frame FRAMES, so the loop closes.
static void morph(const Ball* in, Ball* out, int n, int frame, float phase,
                  float amp) {
	const float t = 6.2831853f * (float)frame / (float)FRAMES;
	for (int i = 0; i < n; ++i) {
		const float ph = phase + 1.7f * (float)i;
		out[i].x = in[i].x + amp * 0.9f * __builtin_cosf(t + ph);
		out[i].y = in[i].y + amp * 0.7f * __builtin_sinf(t + ph * 1.3f);
		out[i].r = in[i].r + amp * __builtin_sinf(t + ph * 0.6f);
	}
}

static const int PULSE_CX = 95, PULSE_CY = 188;

static void rest_pulse(SharpLcd& l, int frame) {
	static const int R[FRAMES] = {4, 6, 8, 10, 8, 6};
	const int r = R[frame % FRAMES];
	for (int dy = -r; dy <= r; ++dy)
		for (int dx = -r; dx <= r; ++dx)
			if (dx * dx + dy * dy <= r * r)
				l.set_pixel((uint16_t)(PULSE_CX + dx), (uint16_t)(PULSE_CY + dy), false);
	const int ring = r + 5;
	for (int a = 0; a < 360; a += 12) {
		const float t = (float)a * 3.14159265f / 180.0f;
		l.set_pixel((uint16_t)(PULSE_CX + (int)(ring * __builtin_cosf(t))),
		            (uint16_t)(PULSE_CY + (int)(ring * __builtin_sinf(t))), false);
	}
}

static void rest_compose(SharpLcd& l, int frame) {
	Ball shell[4], t1[3], t2[3];
	// Amplitude does not affect transfer cost: these shapes put a moved edge
	// on nearly every line they cross, so those lines are dirty either way.
	morph(SHELL0,  shell, 4, frame, 0.0f, 5.0f);
	morph(TILE1_0, t1,    3, frame, 2.1f, 3.5f);
	morph(TILE2_0, t2,    3, frame, 4.2f, 3.5f);

	l.fill_white();
	dot_field(l, 16);

	blob(l, shell, 4, 2, 14, 204, 218, true, true);
	blob_inner_ring(l, shell, 4, 2, 14, 204, 218, 8);

	l.draw_text(58, 60, "thicket", false);
	l.draw_text_scaled(58, 92, "9:22", false, 3);
	l.draw_text(58, 146, "tue 5 aug", false);
	for (int i = 0; i < 64; i += 3) l.set_pixel((uint16_t)(58 + i), 168, false);
	rest_pulse(l, frame);

	blob(l, t1, 3, 202, 16, 398, 116, false, true);
	field_text(l, 238, 44, "2", 3);
	field_text(l, 268, 50, "messages", 1);
	field_text(l, 268, 66, "waiting", 1);

	blob(l, t2, 3, 202, 114, 398, 214, false, true);
	field_text(l, 238, 142, "4", 3);
	field_text(l, 268, 148, "people", 1);
	field_text(l, 268, 164, "reachable", 1);

	gauge(l, 8, 220, 96, 14, 3, 4);
	field_text(l, 112, 221, "3 days", 1);
	field_text(l, 392 - tw("listening"), 221, "listening", 1);
}

static void d_rest(VirtualPanel& p, SharpLcd& l) {
	// Compose off to the side and copy pixel by pixel, so a line is only dirty
	// when it genuinely changed. Clearing and redrawing in place would mark
	// every line it touched and the cost below would be fiction.
	VirtualPanel sink;
	SharpLcd c(sink, compose_fb);

	l.fill_white();
	for (int f = 0; f < FRAMES; ++f) {
		rest_compose(c, f);
		blit(l, c);

		p.reset_counters();
		l.flush();

		char name[32];
		if (f == 0) snprintf(name, sizeof(name), "d5-rest");
		else        snprintf(name, sizeof(name), "d5-rest-f%d", f + 1);
		save(p, name);
		printf("    frame %d: %u lines, %u bytes on the wire\n", f + 1,
		       (unsigned)p.lines_written(), (unsigned)(2 + 52 * p.lines_written()));
	}
}

// Deliberately uneven: two equal lobes at the same height merge into a flat
// top, and the design language calls rigid symmetry off-language anyway.
static const Ball HERO[3] = {
	{126.0f, 120.0f, 56.0f}, {266.0f, 104.0f, 58.0f}, {198.0f, 132.0f, 50.0f},
};

static void d_first_run(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	hero_ground(l);
	blob(l, HERO, 3, 38, 22, 372, 212, false, true);
	status_bar(l, "thicket", "9:22", 100);

	field_text(l, 100, 68, "No one added yet", 2);
	field_text(l, 104, 110, "Hold this next to another thicket", 1);
	field_text(l, 104, 128, "and press the wheel on both.", 1);
	field_text(l, 104, 150, "Nothing is sent until you do.", 1);

	hint_bar(l, "press to add someone", NULL, HINT_Y);
	l.flush(); save(p, "d6-first-run");
}

static void d_queued(VirtualPanel& p, SharpLcd& l) {
	l.fill_white();
	hero_ground(l);
	blob(l, HERO, 3, 38, 22, 372, 212, false, true);
	status_bar(l, "messages", "9:22", 41);

	field_text(l, 100, 68, "Nothing in range", 2);
	field_text(l, 106, 110, "3 messages are waiting to go.", 1);
	field_text(l, 106, 128, "They send as soon as anything", 1);
	field_text(l, 106, 146, "comes within reach.", 1);

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
