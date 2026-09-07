// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: Apache-2.0
//
// See MicronRender.h for why there is no layout tree.

#include "MicronRender.h"

namespace thicket {

namespace {

// Bytes in one UTF-8 sequence, so a run can be walked without decoding it.
inline uint8_t seq_len(uint8_t c) {
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;                      // malformed: one byte, never zero
}

// Codepoints in a run. Cozette advances 6px for every glyph it has and for
// every one it does not, so this IS the width in cells.
size_t cells(const char* t, size_t n) {
	size_t c = 0;
	for (size_t i = 0; i < n; ) { i += seq_len((uint8_t)t[i]); ++c; }
	return c;
}

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

uint8_t PageRenderer::luma_of(const micron::Color& c) {
	if (c.is_default || !c.is_valid) return 255;
	const uint32_t r = (c.rgb >> 16) & 0xff, g = (c.rgb >> 8) & 0xff, b = c.rgb & 0xff;
	return (uint8_t)((299u * r + 587u * g + 114u * b) / 1000u);
}

// A background is drawn as texture carrying its brightness. Ink stays black and
// the rule that overrides everything still holds: whichever way the background
// went, the text on it must read.
void PageRenderer::paint_bg(uint16_t x, uint16_t w, uint8_t luma) {
	if (!row_visible(row_h())) return;
	if (_dither) _lcd.fill_dither(x, screen_y(), w, row_h(), luma);
	else if (luma < 128) _lcd.fill_rect(x, screen_y(), w, row_h(), true);
}

bool PageRenderer::background_is_dark(const micron::Color& c) {
	if (c.is_default || !c.is_valid) return false;
	// Rec. 601 luma, integer. Below half is dark enough that white-on-it reads.
	const uint32_t r = (c.rgb >> 16) & 0xff, g = (c.rgb >> 8) & 0xff, b = c.rgb & 0xff;
	return (299u * r + 587u * g + 114u * b) / 1000u < 128u;
}

void PageRenderer::begin(uint16_t scroll_y) {
	_scroll = scroll_y;
	_y = 0; _x = 0; _row_open = false; _depth = 0; _invert = false;
	_in_table = false; _table_row = 0; _table_cols = 0;
	_link_count = 0; _links_overflowed = false;
	_missing_n = 0; _missing_total = 0; _missing_over = false;
	_blank_pending = true;   // no leading gap at the top of a page
}

void PageRenderer::blankLine() {
	// Collapse runs of blank lines to one gap, the way a reader expects and a
	// page full of trailing whitespace does not deserve.
	if (_blank_pending) return;
	_y = (uint16_t)(_y + line_h());
	_blank_pending = true;
}

void PageRenderer::note_missing(uint32_t cp) {
	++_missing_total;
	for (uint8_t i = 0; i < _missing_n; ++i)
		if (_missing[i].cp == cp) { ++_missing[i].n; return; }
	if (_missing_n < MAX_MISSING) { _missing[_missing_n].cp = cp; _missing[_missing_n].n = 1; ++_missing_n; }
	else _missing_over = true;
}

uint16_t PageRenderer::left_edge() const {
	const uint8_t d = _depth > PageMetrics::MAX_DEPTH ? PageMetrics::MAX_DEPTH : _depth;
	return (uint16_t)(PageMetrics::MARGIN_X + d * PageMetrics::INDENT_PX);
}

bool PageRenderer::row_visible(uint16_t h) const {
	// The top edge must be AT OR BELOW the window, not merely overlapping it.
	// A row straddling the top gives screen_y() a negative value, which is
	// unsigned here and wraps to about 65000; fill_rect's loop counter then
	// wraps with it and never terminates. Scroll offsets are whole rows in
	// practice, so nothing straddles.
	(void)h;
	return _y >= _scroll && (uint32_t)_y < (uint32_t)_scroll + LCD_HEIGHT;
}

uint16_t PageRenderer::screen_y() const { return (uint16_t)(_y - _scroll); }

void PageRenderer::newline() {
	_y = (uint16_t)(_y + row_h());
	// Face and row height are NOT cleared here. newline() also fires on a wrap
	// inside a long heading, and a heading that spills onto a second line must
	// stay the same size across the break rather than shearing to body. The
	// logical line end clears them in onLineEnd.
	_x = left_edge();
	_row_open = false;
}

void PageRenderer::wrap_if_needed(uint16_t next_w) {
	const uint16_t right = LCD_WIDTH - PageMetrics::MARGIN_X;
	if (_x > left_edge() && _x + next_w > right) newline();
}

// Draw a run, wrapping at word boundaries. Nothing is copied: the pen walks
// the caller's bytes and draws a glyph at a time.
// A literal line is drawn as-is and clipped. No wrapping, no hard-breaking:
// both destroy the alignment a preformatted block exists to hold. Wide art
// authored for 80 columns cannot be made right on 66; showing its left portion
// intact with a mark beats showing it in fragments.
void PageRenderer::emit_literal(const char* t, size_t n, bool invert) {
	if (!_row_open) _x = left_edge();
	const uint16_t right = LCD_WIDTH - PageMetrics::MARGIN_X;
	const uint16_t last_cell = (uint16_t)(right - PageMetrics::FONT_ADVANCE);

	size_t k = 0;
	while (k < n) {
		const uint8_t L = seq_len((uint8_t)t[k]);
		if (_x + PageMetrics::FONT_ADVANCE > right) {
			// Something is still to come: mark the last column and stop.
			if (row_visible(row_h())) {
				char m[3] = { (char)0xC2, (char)0xBB, 0 };      // U+00BB
				_lcd.fill_rect(last_cell, screen_y(), PageMetrics::FONT_ADVANCE,
				               row_h(), invert);
				_lcd.draw_text(last_cell, screen_y(), m, !invert);
			}
			_row_open = true;
			return;
		}
		char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
		// Counted whether or not it is on screen: a missing glyph is a fact
		// about the page, not about the current scroll window.
		{ const char* q = g; const uint32_t cp = SharpLcd::next_codepoint(q);
		  if (!SharpLcd::has_glyph(cp)) note_missing(cp); }
		if (row_visible(row_h())) {
			if (invert) _lcd.fill_rect(_x, screen_y(), PageMetrics::FONT_ADVANCE, row_h(), true);
			_lcd.draw_text(_x, screen_y(), g, !invert);   // preformatted is always Cozette
		}
		_x = (uint16_t)(_x + PageMetrics::FONT_ADVANCE);
		_row_open = true;
		k += L;
	}
}

void PageRenderer::emit_run(const char* t, size_t n, bool invert) {
	if (_literal) { emit_literal(t, n, invert); return; }
	// THE choke point for the indent guard. Every glyph this renderer draws
	// passes through here, so resetting unconditionally on a fresh row means no
	// caller can leave the pen somewhere a previous block left it, and no depth
	// can push a row off-panel however deep the page claims to be.
	if (!_row_open) _x = left_edge();
	const uint16_t right = LCD_WIDTH - PageMetrics::MARGIN_X;

	size_t i = 0;
	while (i < n) {
		// One word, plus the spaces in front of it, measured before drawing so
		// the break lands between words and never inside a glyph.
		size_t sp = i;
		while (sp < n && is_space(t[sp])) ++sp;
		size_t w = sp;
		while (w < n && !is_space(t[w])) w += seq_len((uint8_t)t[w]);

		const uint16_t lead_w = (uint16_t)((sp - i) * advance());
		const uint16_t word_w = (uint16_t)(cells(t + sp, w - sp) * advance());

		// A word too long for a whole line is broken at the margin rather than
		// pushed off the panel. Long URLs are the case that matters.
		if (word_w > PageMetrics::content_w() - (left_edge() - PageMetrics::MARGIN_X)) {
			for (size_t k = i; k < w; ) {
				const uint8_t L = seq_len((uint8_t)t[k]);
				wrap_if_needed(advance());
				char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
				const char* q = g; const uint32_t cp = SharpLcd::next_codepoint(q);
				if (!face_has(cp)) note_missing(cp);
				if (row_visible(row_h())) draw_glyph(_x, screen_y(), g, cp, !invert);
				_x = (uint16_t)(_x + advance());
				_row_open = true;
				k += L;
			}
			i = w;
			continue;
		}

		// Leading spaces are dropped at a wrap, never carried to column 0.
		if (_x + lead_w + word_w > right && _x > left_edge()) newline();
		else if (lead_w) {
			// Leading spaces carry the texture: there is no glyph to protect,
			// and on a gradient bar the spaces ARE the bar.
			if (_has_bg) paint_bg(_x, lead_w, _bg_luma);
			_x = (uint16_t)(_x + lead_w);
			// ANYTHING that consumes width opens the row. Only glyphs used to,
			// so a run of pure spaces left the row closed and the next run
			// reset the pen to the margin and painted over it: a sixteen-step
			// gradient drew all sixteen steps in the same 24 pixels.
			_row_open = true;
		}

		for (size_t k = sp; k < w; ) {
			const uint8_t L = seq_len((uint8_t)t[k]);
			char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
			// Counted whether or not it is on screen: a codepoint the font does
			// not carry draws blank and still advances, so the hole is silent
			// and stays aligned. It is a fact about the page, not the window.
			{ const char* q = g; const uint32_t cp = SharpLcd::next_codepoint(q);
			  if (!face_has(cp)) note_missing(cp); }
			if (row_visible(row_h())) {
				// Texture under every cell, then a KNOCKOUT under a glyph only:
				// where a letter sits the cell goes solid so it has full
				// contrast, and the texture survives everywhere else.
				const bool blank = (L == 1 && (g[0] == ' ' || g[0] == '\t'));
				if (_has_bg) {
					if (blank) paint_bg(_x, advance(), _bg_luma);
					else _lcd.fill_rect(_x, screen_y(), advance(), row_h(), invert);
				} else if (invert) {
					_lcd.fill_rect(_x, screen_y(), advance(), row_h(), true);
				}
				if (!blank) {
					const char* q = g; const uint32_t cp = SharpLcd::next_codepoint(q);
					draw_glyph(_x, screen_y(), g, cp, !invert);
				}
				// Underline per cell, so it follows the run across a wrap. A
				// single post-hoc rule underlined only the first row of a link
				// that spilled onto a second line.
				if (_underline)
					_lcd.draw_hline(_x, (uint16_t)(screen_y() + row_h() - 2), advance(), !invert);
			}
			_x = (uint16_t)(_x + advance());
			_row_open = true;
			k += L;
		}
		i = w;
	}
}

namespace {
SharpLcd::BoldFace head_bold_face(uint8_t head) {
	return head == 2 ? SharpLcd::BOLD_H2 : SharpLcd::BOLD_H3;
}
}  // namespace

uint8_t PageRenderer::advance() const {
	if (_head == 1) return SharpLcd::big_text_w();
	if (_head == 2 || _head == 3) return SharpLcd::bold_w(head_bold_face(_head));
	if (_bold) return SharpLcd::bold_w(SharpLcd::BOLD_INLINE);   // 6, same as body
	return PageMetrics::FONT_ADVANCE;
}

bool PageRenderer::face_has(uint32_t cp) const {
	if (_head == 1) return SharpLcd::big_has(cp);
	if (_head == 2 || _head == 3) return SharpLcd::bold_has(cp, head_bold_face(_head));
	return SharpLcd::has_glyph(cp);
}

uint8_t PageRenderer::draw_glyph(uint16_t x, uint16_t y, const char* g, uint32_t cp, bool black) {
	if (_head == 1) { _lcd.draw_text_big(x, y, g, black); return SharpLcd::big_text_w(); }
	if (_head == 2 || _head == 3) {
		const SharpLcd::BoldFace f = head_bold_face(_head);
		_lcd.draw_text_bold(x, y, g, f, black);
		return SharpLcd::bold_w(f);
	}
	if (_bold && SharpLcd::bold_has(cp, SharpLcd::BOLD_INLINE)) {
		// Inline bold sits in the 13px body row. Tamzen 6x12 ascent 10 equals
		// Cozette's, so the same top gives the same baseline.
		_lcd.draw_text_bold(x, y, g, SharpLcd::BOLD_INLINE, black);
		return SharpLcd::bold_w(SharpLcd::BOLD_INLINE);
	}
	_lcd.draw_text(x, y, g, black);       // Cozette body, and the fallback for all
	return PageMetrics::FONT_ADVANCE;
}

void PageRenderer::onText(const char* t, size_t n, const micron::Style& s) {
	_depth = s.depth;
	_blank_pending = false;
	_literal = s.literal;
	// A fresh row restarts at the left edge. Only moving forward to it leaves
	// the pen wherever the previous row ended, which is how a deep-indented
	// divider dragged the following paragraph to the right margin.
	if (!_row_open) _x = left_edge();

	// ALIGNMENT. Centring needs the row's width before the row is drawn, and
	// this renderer holds no row. Where the whole span fits one row the offset
	// is exact; where it wraps, alignment is dropped rather than guessed at,
	// because a half-centred paragraph reads worse than a left-aligned one.
	if (!_row_open && s.align != micron::Align::Left) {
		const uint16_t w = (uint16_t)(cells(t, n) * advance());
		const uint16_t avail = (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - left_edge());
		if (w <= avail) {
			_x = s.align == micron::Align::Center
			   ? (uint16_t)(left_edge() + (avail - w) / 2)
			   : (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - w);
		}
	}

	// A heading is inverted across the full content width, which is the only
	// weight distinction a single-weight font can make.
	_has_bg = !s.bg.is_default && s.bg.is_valid;
	_bg_luma = luma_of(s.bg);
	const bool dark_bg = _has_bg && _bg_luma < 128;
	// Depth 3 and beyond stop inverting: three stacked bars is a page of bars.
	// They get plain text over a rule instead, drawn at end of line.
	// Only depth 1 gets the bar. An inset band at depth 2 is a 24px gap on a
	// 400px bar and does not read; a rule under the text does.
	// A depth-1 heading may take the large face. Only when every codepoint in
	// it is carried there: falling back per glyph would mix two sizes on one
	// line, which is worse than not doing it at all.
	// THE HEADING LADDER. A heading is drawn whole-line in one face:
	//   depth 1  Cozette hi-DPI 12x26
	//   depth 2  Tamzen bold  8x16   (H2)
	//   depth 3+ Tamzen bold  7x13   (H3)
	// Whole-line-or-nothing: if any codepoint on the line is missing from the
	// chosen face, the row drops to Cozette body, so two sizes never share a
	// line and a fallback cannot misalign a heading.
	if (_head2x && s.heading && !_row_open) {
		const uint8_t want = s.depth <= 1 ? 1 : s.depth == 2 ? 2 : 3;
		bool all = n > 0;
		for (size_t k = 0; k < n && all; ) {
			const uint8_t L = seq_len((uint8_t)t[k]);
			uint32_t cp = (uint8_t)t[k];
			if (L == 2) cp = ((uint32_t)(t[k] & 0x1F) << 6) | (t[k+1] & 0x3F);
			else if (L == 3) cp = ((uint32_t)(t[k] & 0x0F) << 12)
			                    | ((uint32_t)(t[k+1] & 0x3F) << 6) | (t[k+2] & 0x3F);
			else if (L > 3) cp = 0xFFFF;
			const bool has = want == 1 ? SharpLcd::big_has(cp)
			               : SharpLcd::bold_has(cp, want == 2 ? SharpLcd::BOLD_H2 : SharpLcd::BOLD_H3);
			if (!has) all = false;
			k += L;
		}
		if (all) {
			_head = want;
			_big = (want == 1);
			const uint8_t ch = want == 1 ? SharpLcd::big_text_h()
			                 : SharpLcd::bold_h(want == 2 ? SharpLcd::BOLD_H2 : SharpLcd::BOLD_H3);
			_row_h = (uint16_t)(ch + _leading);
		}
	}

	// Inline bold, for a run that is NOT a heading. Headings are already a bold
	// face, so bold there is a no-op rather than a double weight. Falls back to
	// Cozette per glyph inside draw_glyph.
	_bold = s.bold && _head == 0;
	_underline = s.underline;

	// No inversion for headings: the ladder carries hierarchy by size and
	// weight. Inversion is reserved for a dark background the PAGE asked for.
	const bool head_bar = false;
	const bool invert = head_bar || dark_bg;
	// H1 is carried by size alone. H2 and deeper get a rule.
	// A rule under a heading is now only the FALLBACK cue. When the Tamzen
	// ladder applied, size and weight carry it and a rule would be a third
	// signal. When the line dropped to body size (a missing glyph), the rule is
	// the only thing left distinguishing it, so it is drawn then.
	_head_rule = _stepped && s.heading && s.depth >= 2 && _head == 0;
	_invert = invert;

	// No row-wide band. A gradient bar is many runs on ONE row, each with its
	// own colour, so the background is painted per CELL as the run is drawn.
	// Painting the row once with the first run's colour is what turned a
	// sixteen-step ramp into one black bar.
	emit_run(t, n, invert);

}

void PageRenderer::onLink(const char* label, size_t label_len,
                          const char* target, size_t target_len,
                          const char* /*fields*/, size_t /*fields_len*/,
                          const micron::Style& s) {
	_depth = s.depth;
	if (!_row_open) _x = left_edge();
	const uint16_t x0 = _x;
	const uint16_t y0 = screen_y();

	// The underline is drawn per cell inside emit_run, so it follows the label
	// across a wrap rather than underlining only the first row.
	const bool prev_ul = _underline;
	_underline = true;
	emit_run(label, label_len, _invert);
	_underline = prev_ul;

	// Recorded for a later input layer. A wrapped link is boxed on its last row
	// only, which is enough to press; the visible underline spans every row.
	if (row_visible(row_h()) && _link_count < MAX_LINKS) {
		LinkBox& b = _links[_link_count++];
		b.x = x0; b.y = y0;
		b.w = (uint16_t)(_x > x0 ? _x - x0 : 0);
		b.h = row_h();
		b.target = target; b.target_len = (uint16_t)target_len;
	} else if (row_visible(row_h())) {
		_links_overflowed = true;
	}
}

void PageRenderer::onDivider(uint32_t ch, const micron::Style& s) {
	_depth = s.depth;
	if (!_row_open) _x = left_edge();
	// The reference fills the row with the character the page asked for, so
	// three dividers with three fills have three textures. The default U+2500
	// still reads as a continuous rule, because the box glyphs are 7px of ink
	// against a 6px advance and neighbouring cells touch.
	if (row_visible(row_h())) {
		char g[5]; uint8_t n = 0;
		if (ch < 0x80) { g[n++] = (char)ch; }
		else if (ch < 0x800) { g[n++] = (char)(0xC0 | (ch >> 6)); g[n++] = (char)(0x80 | (ch & 0x3F)); }
		else { g[n++] = (char)(0xE0 | (ch >> 12)); g[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
		       g[n++] = (char)(0x80 | (ch & 0x3F)); }
		g[n] = 0;
		if (!SharpLcd::has_glyph(ch)) note_missing(ch);
		for (uint16_t x = left_edge(); x + PageMetrics::FONT_ADVANCE <= LCD_WIDTH - PageMetrics::MARGIN_X;
		     x = (uint16_t)(x + PageMetrics::FONT_ADVANCE))
			_lcd.draw_text(x, screen_y(), g, true);
	}
	_row_open = true;
}

void PageRenderer::onField(const micron::Field& f, const micron::Style& s) {
	_depth = s.depth;
	// An input as a bracketed slot of its declared width, so a form reads as a
	// form before any input layer exists.
	const char open = f.kind == micron::FieldKind::Checkbox ? '['
	                : f.kind == micron::FieldKind::Radio    ? '(' : '[';
	const char close = f.kind == micron::FieldKind::Checkbox ? ']'
	                 : f.kind == micron::FieldKind::Radio    ? ')' : ']';
	char box[2] = { open, 0 };
	emit_run(box, 1, _invert);
	if (f.kind == micron::FieldKind::Text) {
		const uint8_t w = f.width ? f.width : 1;
		for (uint8_t k = 0; k < w; ++k) emit_run("_", 1, _invert);
	} else {
		emit_run(f.prechecked ? "x" : " ", 1, _invert);
	}
	box[0] = close;
	emit_run(box, 1, _invert);

	// A text field shows its preset content; a box shows its label. A MASKED
	// field shows neither: the reference passes mask="*" to the edit widget, so
	// the value never reaches the screen, and printing it here would defeat the
	// only thing the flag is for.
	if (f.kind == micron::FieldKind::Text) {
		if (f.value_len) {
			emit_run(" ", 1, _invert);
			if (f.masked) for (size_t k = 0; k < cells(f.value, f.value_len); ++k)
				emit_run("*", 1, _invert);
			else emit_run(f.value, f.value_len, _invert);
		}
	} else if (f.label_len) {
		emit_run(" ", 1, _invert);
		emit_run(f.label, f.label_len, _invert);
	}
}

void PageRenderer::onAnchor(const char* /*name*/, size_t /*len*/) {
	// Zero width by definition. Nothing to draw; it exists for in-page links.
}

void PageRenderer::onLineEnd(const micron::Style& s) {
	_depth = s.depth;
	// A heading's inverted band already covers the row; add a little air after
	// it so the next paragraph does not sit against the bar.
	// A deep heading is ruled rather than barred, so the rule is drawn once the
	// row's width is known, which is here.
	if (_head_rule && _row_open && row_visible(row_h())) {
		// Depth 2 rules the full content width, deeper ones only their own
		// text, so the levels keep stepping down in weight.
		const uint16_t w = s.depth <= 2
			? (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - left_edge())
			: (uint16_t)(_x - left_edge());
		_lcd.draw_hline(left_edge(), (uint16_t)(screen_y() + row_h() - 1), w, true);
	}
	_head_rule = false;

	const bool was_heading = s.heading;
	const bool was_big = (_head == 1);
	newline();
	_row_h = 0;   // next logical line is body height unless it sets its own
	_big = false;
	_head = 0;
	_bold = false;
	// A heading needs air under it, and the big one needs more: at 26 px its
	// baseline otherwise sits hard against the first body line.
	if (was_heading)
		_y = (uint16_t)(_y + (was_big ? PageMetrics::HEAD_GAP_BIG
		                              : PageMetrics::PARA_GAP));
	_invert = false;
}

void PageRenderer::onTableBegin(const micron::Table& t, const micron::Style& s) {
	_depth = s.depth;
	_in_table = true;
	_table_row = 0; _table_cols = 0; _t_rows = 0; _tlen = 0;
	_table_overflowed = false;
	_table_align = t.align_set ? t.align : micron::Align::Left;
	for (uint8_t i = 0; i < T_COLS; ++i) _col_w[i] = 0;
}

// Rows are collected, not drawn. Column widths need the widest cell in each
// column, so nothing can be laid out until the table closes.
void PageRenderer::onTableRow(const char* row, size_t len, const micron::Style& s) {
	_depth = s.depth;
	size_t a = 0, b = len;
	while (a < b && (row[a] == '|' || is_space(row[a]))) ++a;
	while (b > a && (row[b - 1] == '|' || is_space(row[b - 1]))) --b;

	// A separator row carries alignment in markdown and no content here.
	bool sep = b > a;
	for (size_t i = a; i < b && sep; ++i)
		if (row[i] != '-' && row[i] != ':' && row[i] != '|' && !is_space(row[i])) sep = false;
	if (sep) return;

	if (_t_rows >= T_ROWS) { _table_overflowed = true; return; }
	const uint8_t r = _t_rows;
	for (uint8_t k = 0; k < T_COLS; ++k) { _cell_off[r][k] = 0; _cell_len[r][k] = 0; }

	uint8_t col = 0;
	size_t i = a;
	while (i <= b && col < T_COLS) {
		size_t cell = i;
		while (cell < b && row[cell] != '|') ++cell;
		size_t cs = i, ce = cell;
		while (cs < ce && is_space(row[cs])) ++cs;
		while (ce > cs && is_space(row[ce - 1])) --ce;

		// Cells carry inline markup, and the reference re-parses every table
		// line so it is interpreted. Colour and weight are unavailable on this
		// panel anyway, so the commands are STRIPPED here rather than styled:
		// what matters is that the reader sees "Apple" and not "`F3a3Apple`f".
		_cell_off[r][col] = _tlen;
		size_t n = 0;
		for (size_t k = cs; k < ce && n < 255; ) {
			if (row[k] == '`' && k + 1 < ce) {
				const char cmd = row[k + 1];
				size_t skip = 2;
				if (cmd == 'F' || cmd == 'B') {
					// `Frgb, or `FTrrggbb. The reference consumes the digits
					// whatever they are, so the same count is skipped here.
					// backtick + F + three digits = 5; with T, six digits = 9.
					skip = (k + 2 < ce && row[k + 2] == 'T') ? 9 : 5;
				} else if (cmd == '[') {
					// A link's label survives; its target does not belong in a
					// column two words wide.
					size_t e = k + 2, bar = ce;
					while (e < ce && row[e] != ']') { if (row[e] == '`' && bar == ce) bar = e; ++e; }
					const size_t stop = (bar < ce) ? bar : e;
					for (size_t q = k + 2; q < stop && n < 255; ++q) {
						if (_tlen + n < T_BYTES) _tbuf[_tlen + n] = row[q];
						else { _table_overflowed = true; break; }
						++n;
					}
					k = (e < ce) ? e + 1 : ce;
					continue;
				}
				k += (k + skip <= ce) ? skip : (ce - k);
				continue;
			}
			if (_tlen + n < T_BYTES) _tbuf[_tlen + n] = row[k];
			else { _table_overflowed = true; break; }
			++n; ++k;
		}
		_cell_len[r][col] = (uint8_t)n;
		_tlen = (uint16_t)(_tlen + n);

		if (n) {
			const uint8_t w = (uint8_t)cells(_tbuf + _cell_off[r][col], n);
			if (w > _col_w[col]) _col_w[col] = w;
		}
		++col;
		i = cell + 1;
	}
	if (col > _table_cols) _table_cols = col;
	++_t_rows;
}

void PageRenderer::onTableEnd(const micron::Style& s) {
	_depth = s.depth;
	table_flush();
	_in_table = false;
}

// Lay the collected rows into columns and draw them.
//
// Columns take their natural width where the table fits, and are scaled down
// proportionally where it does not, with a floor so no column collapses to
// nothing. A cell wider than its column WRAPS, which makes the row taller; it
// is never clipped, because a clipped cell loses the reader something a taller
// row would have shown.
void PageRenderer::table_flush() {
	if (!_t_rows || !_table_cols) return;

	const uint8_t GAP = 1;                       // a space between columns
	const uint8_t avail = (uint8_t)((LCD_WIDTH - PageMetrics::MARGIN_X - left_edge())
	                                / PageMetrics::FONT_ADVANCE);
	// Cap any single column at half the width before fitting. One long cell
	// otherwise takes everything proportional scaling has to give and starves
	// the rest: a 60-cell price column squeezed Qty to two cells and broke the
	// header across two lines for no reason.
	const uint8_t cap = (uint8_t)(avail / 2);
	for (uint8_t c = 0; c < _table_cols; ++c)
		if (_col_w[c] > cap) _col_w[c] = cap;

	uint16_t natural = 0;
	for (uint8_t c = 0; c < _table_cols; ++c) natural = (uint16_t)(natural + _col_w[c] + GAP);

	if (natural > avail) {
		// Scale to fit. Two cells is the floor: narrower than that and a column
		// is a column of hyphens.
		const uint16_t room = (uint16_t)(avail - _table_cols * GAP);
		uint16_t sum = 0;
		for (uint8_t c = 0; c < _table_cols; ++c) sum = (uint16_t)(sum + _col_w[c]);
		if (!sum) return;
		uint16_t used = 0;
		for (uint8_t c = 0; c < _table_cols; ++c) {
			uint16_t w = (uint16_t)((uint32_t)_col_w[c] * room / sum);
			if (w < 2) w = 2;
			_col_w[c] = (uint8_t)w;
			used = (uint16_t)(used + w);
		}
		// Give any rounding remainder to the widest column rather than losing it.
		if (used < room) {
			uint8_t widest = 0;
			for (uint8_t c = 1; c < _table_cols; ++c) if (_col_w[c] > _col_w[widest]) widest = c;
			_col_w[widest] = (uint8_t)(_col_w[widest] + (room - used));
		}
	}

	for (uint8_t r = 0; r < _t_rows; ++r) {
		// How many lines the tallest cell in this row needs.
		uint8_t lines = 1;
		for (uint8_t c = 0; c < _table_cols; ++c) {
			if (!_cell_len[r][c] || !_col_w[c]) continue;
			// Counted by walking with the SAME break rule the draw uses. A
			// word break can push a cell onto one more line than a plain
			// divide predicts, and a row too short overlaps the next.
			const char* p = _tbuf + _cell_off[r][c];
			const size_t n = _cell_len[r][c];
			uint8_t need = 0;
			size_t at = 0;
			while (at < n && need < 32) {
				size_t e = at, t2 = 0;
				while (e < n && t2 < _col_w[c]) { e += seq_len((uint8_t)p[e]); ++t2; }
				if (e < n && p[e] != ' ') {
					size_t brk = e;
					while (brk > at && p[brk - 1] != ' ') --brk;
					if (brk > at && (brk - at) * 3 >= (e - at) * 2) e = brk;
				}
				at = e;
				while (at < n && p[at] == ' ') ++at;
				++need;
			}
			if (need > lines) lines = need;
		}

		for (uint8_t ln = 0; ln < lines; ++ln) {
			if (row_visible(line_h())) {
				uint16_t x = left_edge();
				for (uint8_t c = 0; c < _table_cols; ++c) {
					// The slice of this cell belonging to line `ln`.
					const char* p = _tbuf + _cell_off[r][c];
					const size_t n = _cell_len[r][c];
					// Walk to this line's slice. A cell with fewer lines than
					// the tallest in the row contributes NOTHING here; without
					// that check a short cell reprints itself on every
					// continuation line.
					// Re-walk from the start each line, applying the same
					// break rule, so a line begins where the previous one
					// actually ended rather than at a fixed multiple.
					size_t from = 0;
					for (uint8_t pass = 0; pass < ln; ++pass) {
						size_t e = from, t2 = 0;
						while (e < n && t2 < _col_w[c]) { e += seq_len((uint8_t)p[e]); ++t2; }
						if (e < n && p[e] != ' ') {
							size_t brk = e;
							while (brk > from && p[brk - 1] != ' ') --brk;
							if (brk > from && (brk - from) * 3 >= (e - from) * 2) e = brk;
						}
						from = e;
						while (from < n && p[from] == ' ') ++from;
						if (from >= n) break;
					}
					size_t to = from, taken = 0;
					while (to < n && taken < _col_w[c]) { to += seq_len((uint8_t)p[to]); ++taken; }
					// Back up to a space rather than break a word, but only if
					// that leaves most of the line used: in a narrow column a
					// word break can waste more than it saves.
					if (to < n && p[to] != ' ') {
						size_t brk = to;
						while (brk > from && p[brk - 1] != ' ') --brk;
						if (brk > from && (brk - from) * 3 >= (to - from) * 2) to = brk;
					}
					if (from < to) {
						_x = x;
						_row_open = true;          // x is the column, not the margin
						emit_run(p + from, to - from, false);
					}
					x = (uint16_t)(x + (_col_w[c] + GAP) * PageMetrics::FONT_ADVANCE);
				}
			}
			_y = (uint16_t)(_y + line_h());
			_row_open = false;
			_x = left_edge();
		}

		// The header is RULED, not inverted: inversion means a dark background
		// the page asked for, everywhere else in this renderer.
		if (r == 0 && row_visible(line_h()))
			_lcd.draw_hline(left_edge(), (uint16_t)(screen_y() - 1),
			                (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - left_edge()), true);
	}

	if (row_visible(line_h()))
		_lcd.draw_hline(left_edge(), screen_y(),
		                (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - left_edge()), true);
	_y = (uint16_t)(_y + PageMetrics::PARA_GAP);
}

void PageRenderer::onImage(const micron::Image& i, const micron::Style& s) {
	_depth = s.depth;
	// Phase 1: no decode. A marker and the alt text, so the reader knows an
	// image is here and what it was meant to show.
	emit_run("[img] ", 6, _invert);
	if (i.alt_len) emit_run(i.alt, i.alt_len, _invert);
	else           emit_run(i.url, i.url_len, _invert);
}

void PageRenderer::onPartial(const micron::Partial& p, const micron::Style& s) {
	_depth = s.depth;
	// Phase 1: no fetch. Shown as a link to the thing that would be pulled in.
	emit_run("[+] ", 4, _invert);
	onLink(p.url, p.url_len, p.url, p.url_len, nullptr, 0, s);
}

}  // namespace thicket
