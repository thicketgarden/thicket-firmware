// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE single page-composition path.
//
// A Micron page becomes a panel frame in exactly one place, and this is it.
// The device UI, the render-to-PNG harness and the desktop browser all call
// compose_page(); none of them re-implements the parse-and-lay-out loop. That
// is the whole point of the split: what one target shows on the panel, the
// others show too, because the pixels come from the same code. Only the
// display backend behind `lcd` and where `src` came from differ per target.
//
// Header-only and free of std::, so the device can call it as readily as a
// host tool can.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "Micron.h"
#include "SharpLcd.h"
#include "MicronRender.h"

namespace thicket {

// Re-flow `src` (len bytes, newline-separated Micron) into the panel at
// `scroll` pixels down, and return the total laid-out height. A blank source
// line is fed as blankLine(), matching the reference, which is why the caller
// hands over raw bytes rather than pre-split lines.
inline uint16_t compose_page(SharpLcd& lcd, PageRenderer& r,
                             const char* src, size_t len, uint16_t scroll) {
	lcd.fill_white();
	micron::Parser p; p.reset(); r.begin(scroll);
	size_t i = 0;
	while (i <= len) {
		size_t e = i;
		while (e < len && src[e] != '\n') ++e;
		if (e == i) r.blankLine();
		else        p.parseLine(src + i, e - i, r);
		if (e >= len) break;
		i = e + 1;
	}
	lcd.flush();
	return r.content_height();
}

}  // namespace thicket
