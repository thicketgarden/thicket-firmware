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
	// INDENT CEILING, a guard rather than a patch. Micron puts no ceiling on
	// section depth, and a page arriving over LoRa can carry any depth at all,
	// whether by malformed markup or by a node that means it. The Guide's own
	// display test already opens a depth-20 heading. Twenty levels is 240 px of
	// indent on a 400 px panel: content walks off the right edge and the reader
	// gets nothing.
	//
	// Four levels is 48 px and still leaves 58 columns. Every indent in this
	// renderer goes through left_edge(), which clamps here, so no page can push
	// content off-panel however deep it claims to be. The pen also restarts at
	// the left edge on every new block, so a runaway x cannot be inherited by
	// the row after it.
	static const uint8_t  MAX_DEPTH  = 4;
	static const uint16_t PARA_GAP   = 4;    // after a divider or a small heading
	static const uint16_t HEAD_GAP_BIG = 9;  // after the 26px heading, which
	                                         // otherwise sits on the body
	static const uint16_t RULE_INSET = 1;    // divider inset from the margin
	// Drawn in the last column of a clipped literal line. Latin-1, so it is in
	// the bundled font, and it reads as "there is more to the right".
	static const uint32_t CLIP_MARK  = 0x00BB;   // >>

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

	// Two knobs, both set once at construction so a comparison render costs a
	// flag rather than a rebuild.
	//   extra_leading: pixels added to every row. Cozette's 13px cell is tight
	//                  for long-form reading and there is vertical room to
	//                  spend: 18 rows at 13px, 16 at 15px.
	//   stepped_heads: distinguish heading levels from each other. Depth 1 is a
	//                  full-width inverted band, depth 2 a band inset to its
	//                  indent, depth 3 and beyond plain text over a rule. With
	//                  this off every level is a full-width band and they are
	//                  indistinguishable.
	// +2 px of leading is the shipped default: 16 rows instead of 18, and a
	// dense page reads markedly better for the two it costs.
	static const uint8_t DEFAULT_LEADING = 2;

	// head_2x draws a depth-1 heading in Cozette hi-DPI 12x26. It REPLACES the
	// inversion bar rather than supplementing it: size carries H1, rules carry
	// H2 and deeper, and nothing inverts for hierarchy at all.
	explicit PageRenderer(SharpLcd& lcd, uint8_t extra_leading = DEFAULT_LEADING,
	                      bool stepped_heads = true, bool head_2x = true)
		: _lcd(lcd), _leading(extra_leading), _stepped(stepped_heads), _head2x(head_2x) {}

	uint16_t line_h() const { return (uint16_t)(PageMetrics::LINE_H + _leading); }
	// The row being laid out may be taller than a body row when a heading is
	// drawn in the large face.
	uint16_t row_h() const { return _row_h ? _row_h : line_h(); }
	uint8_t  advance() const { return _big ? SharpLcd::big_text_w() : PageMetrics::FONT_ADVANCE; }

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

	// MISSING GLYPHS. A codepoint the font does not carry draws blank and still
	// advances, so a page loses characters silently and stays aligned. Counting
	// them is the only way a hole becomes visible without eyes on every render.
	static const uint8_t MAX_MISSING = 24;
	uint8_t  missing_kinds() const { return _missing_n; }
	uint32_t missing_cp(uint8_t i) const { return _missing[i].cp; }
	uint16_t missing_count(uint8_t i) const { return _missing[i].n; }
	uint32_t missing_total() const { return _missing_total; }
	bool missing_overflowed() const { return _missing_over; }
	// A table larger than the fixed buffer. Reported, never silent.
	bool table_overflowed() const { return _table_overflowed; }

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
	// OVERFLOW IS CONTENT-TYPE DEPENDENT.
	//   prose      wraps at a word boundary, and hard-breaks a token that has
	//              no boundary to wrap at.
	//   literal    never wraps. An over-wide line is clipped at the margin with
	//              an indicator in the last column, because wrapping ASCII art
	//              shears it and breaking a token mid-art corrupts the same
	//              alignment the block exists to preserve.
	void emit_run(const char* t, size_t n, bool invert);
	void emit_literal(const char* t, size_t n, bool invert);
	void wrap_if_needed(uint16_t next_w);
	void newline();
	uint16_t left_edge() const;
	bool row_visible(uint16_t h) const;
	uint16_t screen_y() const;   // virtual y mapped into the panel

	SharpLcd& _lcd;
	uint8_t   _leading = 0;
	bool      _stepped = true;
	bool      _head2x = false;
	bool      _big = false;      // this row draws in the large face
	uint16_t  _row_h = 0;        // height of the row being laid out
	uint16_t  _scroll = 0;
	uint16_t  _y = 0;            // virtual y of the current row, page coords
	uint16_t  _x = 0;            // pen x in panel coords
	bool      _row_open = false; // something has been drawn on this row
	uint8_t   _depth = 0;
	bool      _invert = false;   // current row is a dark-background block
	bool      _head_rule = false;
	bool      _literal = false;   // this row is inside a `= block
	bool      _blank_pending = true;  // a blank gap is already open

	// TABLE STATE, in a bounded buffer.
	//
	// Laying a table out needs its widest cell per column, which needs the
	// whole table, which is the one thing this renderer otherwise refuses to
	// hold. A table is not a page though: it is a handful of short cells, so a
	// FIXED buffer holds it and anything past the bound is reported rather
	// than silently truncated. Clipping a cell loses data the reader needed;
	// wrapping it costs a taller row and nothing else.
	static const uint8_t  T_ROWS = 16;
	static const uint8_t  T_COLS = 6;      // more than six is unreadable at 66 cells
	static const uint16_t T_BYTES = 1024;

	bool     _in_table = false;
	uint8_t  _table_row = 0;
	uint8_t  _table_cols = 0;
	uint8_t  _col_w[T_COLS] = {0};         // in cells, after fitting
	micron::Align _table_align = micron::Align::Left;

	char     _tbuf[T_BYTES];
	uint16_t _tlen = 0;
	uint16_t _cell_off[T_ROWS][T_COLS];
	uint8_t  _cell_len[T_ROWS][T_COLS];
	uint8_t  _t_rows = 0;
	bool     _table_overflowed = false;

	void table_flush();

	LinkBox _links[MAX_LINKS];
	uint8_t _link_count = 0;
	bool    _links_overflowed = false;

	struct Missing { uint32_t cp; uint16_t n; };
	Missing  _missing[MAX_MISSING];
	uint8_t  _missing_n = 0;
	uint32_t _missing_total = 0;
	bool     _missing_over = false;
	void note_missing(uint32_t cp);
};

}  // namespace thicket
