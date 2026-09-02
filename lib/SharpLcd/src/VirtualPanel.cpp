// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VirtualPanel.h"

#include <stdio.h>
#include <string.h>

#ifndef ARDUINO
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace thicket {

#ifndef ARDUINO
// Create the parent directories of `path`. The output directory isn't tracked
// by git, so it won't exist in a fresh checkout.
static void make_parent_dirs(const char* path) {
	char buf[512];
	const size_t n = strlen(path);
	if (n >= sizeof(buf)) return;
	memcpy(buf, path, n + 1);
	for (char* p = buf + 1; *p; ++p) {
		if (*p != '/') continue;
		*p = '\0';
		mkdir(buf, 0755);   // EEXIST is the normal case
		*p = '/';
	}
}
#endif

VirtualPanel::VirtualPanel()
	: _tx_len(0), _selected(false), _vcom(false),
	  _lines_written(0), _clears(0), _vcom_toggles(0), _overflowed(false) {
	memset(_img, 0xFF, sizeof(_img));   // panel powers up white
}

void VirtualPanel::reset_counters() {
	_lines_written = 0;
	_clears = 0;
	_vcom_toggles = 0;
}

void VirtualPanel::select(bool on) {
	if (on) { _selected = true; _tx_len = 0; return; }
	_selected = false;
	apply_transaction();
	_tx_len = 0;
}

void VirtualPanel::write(const uint8_t* data, size_t len) {
	if (!_selected) return;             // real panel ignores data with SCS low
	for (size_t i = 0; i < len; ++i) {
		// Truncating here would silently corrupt the image & look like a
		// driver bug, so it's an assertion rather than a clamp.
		if (_tx_len >= sizeof(_tx)) { _overflowed = true; return; }
		_tx[_tx_len++] = data[i];
	}
}

void VirtualPanel::apply_transaction() {
	if (_tx_len == 0) return;

	const uint8_t mode = _tx[0];

	const bool want_vcom = (mode & LCD_M1_VCOM) != 0;
	if (want_vcom != _vcom) { _vcom = want_vcom; ++_vcom_toggles; }

	if (mode & LCD_M2_CLEAR) {
		memset(_img, 0xFF, sizeof(_img));
		++_clears;
		return;
	}
	if ((mode & LCD_M0_WRITE) == 0) return;   // display mode, image unchanged

	// Each line is address + LCD_LINE_BYTES + one gap byte; a final trailer
	// byte closes the transaction.
	size_t i = 1;
	while (_tx_len - i > 1 + LCD_LINE_BYTES) {
		const uint8_t addr = _tx[i++];
		const uint16_t line = SharpLcd::reverse_bits(addr);   // 1-based
		if (line >= 1 && line <= LCD_HEIGHT) {
			memcpy(&_img[(uint32_t)(line - 1) * LCD_LINE_BYTES],
			       &_tx[i], LCD_LINE_BYTES);
			++_lines_written;
		}
		i += LCD_LINE_BYTES;
		++i;                                   // gap
	}
}

bool VirtualPanel::pixel(uint16_t x, uint16_t y) const {
	if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return false;
	const uint8_t b = _img[(uint32_t)y * LCD_LINE_BYTES + (x >> 3)];
	return (b & (uint8_t)(0x80u >> (x & 7))) == 0;
}

bool VirtualPanel::write_pbm(const char* path, uint8_t scale) const {
	if (scale == 0) scale = 1;
#ifndef ARDUINO
	make_parent_dirs(path);
#endif
	FILE* f = fopen(path, "wb");
	if (!f) return false;

	const uint32_t w = (uint32_t)LCD_WIDTH * scale;
	const uint32_t h = (uint32_t)LCD_HEIGHT * scale;
	fprintf(f, "P1\n# LS027B7DH01 virtual panel\n%u %u\n", w, h);

	for (uint16_t y = 0; y < LCD_HEIGHT; ++y) {
		for (uint8_t sy = 0; sy < scale; ++sy) {
			for (uint16_t x = 0; x < LCD_WIDTH; ++x) {
				const char c = pixel(x, y) ? '1' : '0';
				for (uint8_t sx = 0; sx < scale; ++sx) fputc(c, f);
			}
			fputc('\n', f);
		}
	}
	fclose(f);
	return true;
}

}  // namespace thicket
