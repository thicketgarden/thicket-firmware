// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lay a Micron page out on the Sharp panel.
//
// WHAT THIS IS AND IS NOT
//
// micron-cpp reports structure and never resolves anything: it hands over the
// colour a page asked for, the depth of a heading, the rows of a table. This
// is where those become pixels, because this is the only layer that knows the
// panel is 400x240 with one ink and a 6x13 cell.
//
// NO LAYOUT TREE, NO SECOND BUFFER. The page is re-flowed from its source on
// every frame and only the rows falling inside the scroll window are drawn.
// That costs a re-parse per scroll step and saves the entire page model, which
// is the right trade on a part where the framebuffer is already the largest
// allocation in the device.
//
// The only per-page state is a fixed link table, so a later input layer can
// hit-test without a second pass.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Micron.h"
#include "SharpLcd.h"

namespace thicket {

// Layout constants. Kept here rather than inline so the decisions log and the
// code cannot drift apart.
struct PageMetrics {
	static const uint16_t MARGIN_X   = 2;    // left and right
	static const uint16_t LINE_H     = 13;   // FONT_H; 18 rows on a 240px panel
	static const uint16_t INDENT_PX  = 12;   // per section depth: 2 cells
	static const uint16_t PARA_GAP   = 4;    // after a divider or a heading
	static const uint16_t RULE_INSET = 1;    // divider inset from the margin

	static uint16_t content_w() { return LCD_WIDTH - 2 * MARGIN_X; }   // 396
	static uint16_t cols()      { return content_w() / FONT_ADVANCE; } // 66
	static const uint8_t FONT_ADVANCE = 6;
};

// Where a link landed, so input can hit-test later without re-flowing.
struct LinkBox {
	uint16_t x, y, w, h;      // panel coordinates, already scrolled
	const char* target;       // points into the PAGE SOURCE, which must outlive
	uint16_t    target_len;   // the renderer
};

class PageRenderer : public micron::Renderer {
public:
	static const uint8_t MAX_LINKS = 48;

	PageRenderer(SharpLcd& lcd) : _lcd(lcd) {}

	// Re-flow from the top with this scroll offset. Call, then feed every line
	// of the page to a micron::Parser pointed at this renderer.
	void begin(uint16_t scroll_y);

	// An EMPTY source line. The parser reports no row for one, correctly: the
	// reference renders nothing there either. But a page whose paragraphs run
	// together is much worse to read than one with air in it, and the caller is
	// the only layer that still has the source. Call this for a blank line.
	void blankLine();

	// Total laid-out height in pixels, valid after a full pass. Use it to clamp
	// scrolling; it is why a pass is run even when nothing is visible.
	uint16_t content_height() const { return _y; }

	uint8_t link_count() const { return _link_count; }
	const LinkBox& link(uint8_t i) const { return _links[i]; }
	bool links_overflowed() const { return _links_overflowed; }

	// micron::Renderer
	void onText(const char* t, size_t n, const micron::Style& s) override;
	void onLink(const char* label, size_t label_len,
	            const char* target, size_t target_len,
	            const char* fields, size_t fields_len,
	            const micron::Style& s) override;
	void onDivider(uint32_t ch, const micron::Style& s) override;
	void onField(const micron::Field& f, const micron::Style& s) override;
	void onAnchor(const char* name, size_t len) override;
	void onLineEnd(const micron::Style& s) override;
	void onTableBegin(const micron::Table& t, const micron::Style& s) override;
	void onTableRow(const char* row, size_t len, const micron::Style& s) override;
	void onTableEnd(const micron::Style& s) override;
	void onImage(const micron::Image& i, const micron::Style& s) override;
	void onPartial(const micron::Partial& p, const micron::Style& s) override;

private:
	// COLOUR TO INK. The panel has one ink, so a requested colour cannot be
	// reproduced and must not be allowed to destroy legibility. Foreground is
	// always black. A background is honoured only as inversion, and only when
	// it is dark enough to read white text on. Anything else is ignored, which
	// is the safe direction: normal text on white always reads.
	static bool background_is_dark(const micron::Color& c);

	// Draw one codepoint run without copying it. Cozette advances 6px for every
	// glyph, so width is codepoints * 6 and no measuring pass is needed.
	void emit_run(const char* t, size_t n, bool invert);
	void wrap_if_needed(uint16_t next_w);
	void newline();
	uint16_t left_edge() const;
	bool row_visible(uint16_t h) const;
	uint16_t screen_y() const;   // virtual y mapped into the panel

	SharpLcd& _lcd;
	uint16_t  _scroll = 0;
	uint16_t  _y = 0;            // virtual y of the current row, page coords
	uint16_t  _x = 0;            // pen x in panel coords
	bool      _row_open = false; // something has been drawn on this row
	uint8_t   _depth = 0;
	bool      _invert = false;   // current row is a dark-background block
	bool      _blank_pending = true;  // a blank gap is already open

	// Table state. Rows are laid into columns here because the parser refuses
	// to, and a monochrome 66-cell panel is not a 100-column terminal.
	bool     _in_table = false;
	uint8_t  _table_row = 0;
	uint8_t  _table_cols = 0;
	uint8_t  _col_w[8] = {0};    // in cells; 8 columns is already unreadable
	micron::Align _table_align = micron::Align::Left;

	LinkBox _links[MAX_LINKS];
	uint8_t _link_count = 0;
	bool    _links_overflowed = false;
};

}  // namespace thicket
