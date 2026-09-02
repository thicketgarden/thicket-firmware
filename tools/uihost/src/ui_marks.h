// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fruiting-body marks at three sizes.
//
// Each size is drawn pixel by pixel rather than scaled down from the largest.
// Below roughly eight pixels, generating these shapes from a curve or a field
// function degenerates into a bar or a diamond, and downscaling drops the
// features that make the silhouette readable.

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

// Draws the mark standing on `base_y`: its bottom row sits on that line.
//
// `ink` is the colour of the body. Pass false to knock the mark out of a
// filled selection band; gills and stipple invert with it.
//
// `faded` stipples the body instead of filling it, denoting a peer that is
// present but not currently reachable. Stipple rather than an outline: an
// outline needs an edge several pixels thick to read, which the smaller
// sizes don't have.
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

// Corner sprig. Curved stem, three leaves; drawn big enough to read.
static inline void sprig(SharpLcd& l, int x, int y, int dir) {
	for (int i = 0; i < 16; ++i) {
		const int sy = y - (i * i) / 26;
		l.set_pixel((uint16_t)(x + i * dir), (uint16_t)sy, true);
		if (i != 4 && i != 9 && i != 14) continue;
		const int up = (i == 9) ? -1 : 1;
		for (int k = 1; k <= 4; ++k)
			for (int t = 0; t < 2; ++t)
				l.set_pixel((uint16_t)(x + (i + k / 2) * dir + t * dir),
				            (uint16_t)(sy + up * k), true);
	}
}

// Marks a peer with something waiting. Presence only, not a count.
static inline void draw_spore(SharpLcd& l, int cx, int cy, bool ink) {
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
			if (dx * dx + dy * dy <= 5)
				l.set_pixel((uint16_t)(cx + dx), (uint16_t)(cy + dy), ink);
}

}   // namespace thicket
