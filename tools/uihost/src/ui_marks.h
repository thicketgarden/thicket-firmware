// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fruiting-body marks at three sizes, hand-drawn. Below roughly eight pixels an
// equation gives a bar or a diamond rather than the shape meant, so the small
// sizes are placed pixel by pixel instead of scaled down from the large one.

#pragma once

#include "SharpLcd.h"

#include <stdint.h>

namespace thicket {

struct MarkArt {
	const uint32_t* rows;
	int h;
	int w;
	int gill_row;   // -1 where the size is too small for gills to read
};

// 24 x 20. Two states, cap swollen and relaxed, so the mark can breathe.
static const uint32_t MARK_L_ROWS[20] = {
	0x007E00, 0x01FF80, 0x07FFE0, 0x0FFFF0,
	0x1FFFF8, 0x3FFFFC, 0x7FFFFE, 0x7FFFFE,
	0x3FFFFC, 0x01FF80, 0x007E00, 0x007E00,
	0x007E00, 0x007E00, 0x007E00, 0x00FF00,
	0x00FF00, 0x01FF80, 0x03FFC0, 0x07FFE0,
};

static const uint32_t MARK_L_BREATH_ROWS[20] = {
	0x000000, 0x00FC00, 0x03FF00, 0x0FFFC0,
	0x1FFFE0, 0x3FFFF0, 0x7FFFF8, 0x7FFFF8,
	0x3FFFF0, 0x03FF00, 0x00FC00, 0x007E00,
	0x007E00, 0x007E00, 0x007E00, 0x007E00,
	0x00FF00, 0x00FF00, 0x01FF80, 0x03FFC0,
};

// 16 x 14
static const uint32_t MARK_M_ROWS[14] = {
	0x07E0, 0x1FF8, 0x3FFC, 0x7FFE,
	0xFFFF, 0xFFFF, 0x7FFE, 0x07E0,
	0x03C0, 0x03C0, 0x03C0, 0x07E0,
	0x0FF0, 0x1FF8,
};

// 12 x 10
static const uint32_t MARK_S_ROWS[10] = {
	0x0F0, 0x3FC, 0x7FE, 0xFFF,
	0x7FE, 0x0F0, 0x060, 0x060,
	0x0F0, 0x1F8,
};

static const MarkArt MARK_L        = { MARK_L_ROWS,        20, 24, 8 };
static const MarkArt MARK_L_BREATH = { MARK_L_BREATH_ROWS, 20, 24, 8 };
static const MarkArt MARK_M        = { MARK_M_ROWS,        14, 16, 6 };
static const MarkArt MARK_S        = { MARK_S_ROWS,        10, 12, -1 };

static inline bool mark_on(const MarkArt& m, int row, int col) {
	if (row < 0 || row >= m.h || col < 0 || col >= m.w) return false;
	return (m.rows[row] & (1u << (m.w - 1 - col))) != 0;
}

// Draws the mark standing on `base_y` — its bottom row sits on that line.
// `ink` is the colour of the body, so a mark knocked out of a selection band
// passes false and everything inverts with it. `faded` stipples the body,
// which is how someone not heard from today is drawn: still there, not solid.
// An outline was tried first and came out as a scribble at twelve pixels —
// stipple holds the silhouette at any size because it does not depend on the
// edge being thick enough to read.
static inline void draw_mark(SharpLcd& l, const MarkArt& m, int x, int base_y,
                             bool ink, bool faded) {
	const int top = base_y - m.h;
	for (int row = 0; row < m.h; ++row) {
		for (int col = 0; col < m.w; ++col) {
			if (!mark_on(m, row, col)) continue;
			if (faded) {
				if ((col + row) & 1) continue;
			} else if (row == m.gill_row && ((col + row) & 1)) {
				l.set_pixel((uint16_t)(x + col), (uint16_t)(top + row), !ink);
				continue;
			}
			l.set_pixel((uint16_t)(x + col), (uint16_t)(top + row), ink);
		}
	}
}

// Something waiting: a spore lifting off the cap. Not a count — the fact that
// there is anything at all is what the reader needs.
static inline void draw_spore(SharpLcd& l, int cx, int cy, bool ink) {
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
			if (dx * dx + dy * dy <= 5)
				l.set_pixel((uint16_t)(cx + dx), (uint16_t)(cy + dy), ink);
}

}   // namespace thicket
