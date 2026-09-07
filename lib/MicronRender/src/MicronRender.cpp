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
	_blank_pending = true;   // no leading gap at the top of a page
}

void PageRenderer::blankLine() {
	// Collapse runs of blank lines to one gap, the way a reader expects and a
	// page full of trailing whitespace does not deserve.
	if (_blank_pending) return;
	_y = (uint16_t)(_y + line_h());
	_blank_pending = true;
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
	_row_h = 0;
	_big = false;
	_x = left_edge();
	_row_open = false;
}

void PageRenderer::wrap_if_needed(uint16_t next_w) {
	const uint16_t right = LCD_WIDTH - PageMetrics::MARGIN_X;
	if (_x > left_edge() && _x + next_w > right) newline();
}

// Draw a run, wrapping at word boundaries. Nothing is copied: the pen walks
// the caller's bytes and draws a glyph at a time.
void PageRenderer::emit_run(const char* t, size_t n, bool invert) {
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
				if (row_visible(row_h())) {
					char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
					if (_big) _lcd.draw_text_big(_x, screen_y(), g, !invert);
					else      _lcd.draw_text(_x, screen_y(), g, !invert);
				}
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
			if (row_visible(row_h()) && invert)
				_lcd.fill_rect(_x, screen_y(), lead_w, PageMetrics::LINE_H, true);
			_x = (uint16_t)(_x + lead_w);
		}

		for (size_t k = sp; k < w; ) {
			const uint8_t L = seq_len((uint8_t)t[k]);
			if (row_visible(row_h())) {
				char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
				if (invert) _lcd.fill_rect(_x, screen_y(), PageMetrics::FONT_ADVANCE,
				                           PageMetrics::LINE_H, true);
				if (_big) _lcd.draw_text_big(_x, screen_y(), g, !invert);
				else      _lcd.draw_text(_x, screen_y(), g, !invert);
			}
			_x = (uint16_t)(_x + advance());
			_row_open = true;
			k += L;
		}
		i = w;
	}
}

void PageRenderer::onText(const char* t, size_t n, const micron::Style& s) {
	_depth = s.depth;
	_blank_pending = false;
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
	const bool dark_bg = background_is_dark(s.bg);
	// Depth 3 and beyond stop inverting: three stacked bars is a page of bars.
	// They get plain text over a rule instead, drawn at end of line.
	// Only depth 1 gets the bar. An inset band at depth 2 is a 24px gap on a
	// 400px bar and does not read; a rule under the text does.
	// A depth-1 heading may take the large face. Only when every codepoint in
	// it is carried there: falling back per glyph would mix two sizes on one
	// line, which is worse than not doing it at all.
	if (_head2x && s.heading && s.depth <= 1 && !_row_open) {
		bool all = n > 0;
		for (size_t k = 0; k < n && all; ) {
			const uint8_t L = seq_len((uint8_t)t[k]);
			uint32_t cp = (uint8_t)t[k];
			if (L == 2) cp = ((uint32_t)(t[k] & 0x1F) << 6) | (t[k+1] & 0x3F);
			else if (L > 2) cp = 0xFFFF;
			if (!SharpLcd::big_has(cp)) all = false;
			k += L;
		}
		if (all) { _big = true; _row_h = (uint16_t)(SharpLcd::big_text_h() + _leading); }
	}
	// NO INVERSION FOR HEADINGS. Size carries H1 and rules carry the rest; a
	// bar underneath either is a third signal saying the same thing. Inversion
	// is reserved for a dark background the PAGE asked for, which is content
	// rather than hierarchy.
	const bool head_bar = false;
	const bool invert = head_bar || dark_bg;
	// H1 is carried by size alone. H2 and deeper get a rule.
	_head_rule = _stepped && s.heading && s.depth >= 2;
	_invert = invert;

	if (invert && !_row_open && row_visible(row_h())) {
		// Depth 1 takes the full width; depth 2 a band inset to its own indent,
		// so the levels read as different rather than as the same bar. A dark
		// background block always takes the full width: that is the page's own
		// colour, not a heading level.
		const bool inset = _stepped && head_bar && !dark_bg && s.depth >= 2;
		const uint16_t bx = inset ? left_edge() : PageMetrics::MARGIN_X;
		_lcd.fill_rect(bx, screen_y(), (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - bx),
		               row_h(), true);
	}
	emit_run(t, n, invert);

	// Underline stands in for the parser's underline AND for nothing else; the
	// font has no second weight to spend.
	if (s.underline && _row_open && row_visible(row_h()))
		_lcd.draw_hline(left_edge(), (uint16_t)(screen_y() + row_h() - 1),
		                (uint16_t)(_x - left_edge()), !invert);
}

void PageRenderer::onLink(const char* label, size_t label_len,
                          const char* target, size_t target_len,
                          const char* /*fields*/, size_t /*fields_len*/,
                          const micron::Style& s) {
	_depth = s.depth;
	if (!_row_open) _x = left_edge();
	const uint16_t x0 = _x;
	const uint16_t y0 = screen_y();

	emit_run(label, label_len, _invert);

	// Underlined so a link reads as one without colour, and recorded so input
	// can hit-test later. A link that wrapped is boxed on its last row only,
	// which is enough to press.
	if (row_visible(row_h())) {
		const uint16_t x1 = _x;
		if (x1 > x0)
			_lcd.draw_hline(x0, (uint16_t)(y0 + line_h() - 2),
			                (uint16_t)(x1 - x0), !_invert);
		if (_link_count < MAX_LINKS) {
			LinkBox& b = _links[_link_count++];
			b.x = x0; b.y = y0;
			b.w = (uint16_t)(x1 > x0 ? x1 - x0 : 0);
			b.h = row_h();
			b.target = target; b.target_len = (uint16_t)target_len;
		} else {
			_links_overflowed = true;
		}
	}
}

void PageRenderer::onDivider(uint32_t /*ch*/, const micron::Style& s) {
	_depth = s.depth;
	if (!_row_open) _x = left_edge();
	// A rule, not a row of glyphs. The parser reports the fill character the
	// page asked for; a 1px line reads better on this panel than any of them.
	if (row_visible(row_h())) {
		const uint16_t y = (uint16_t)(screen_y() + row_h() / 2);
		_lcd.draw_hline((uint16_t)(left_edge() + PageMetrics::RULE_INSET), y,
		                (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X
		                           - PageMetrics::RULE_INSET - left_edge()), true);
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
	if (f.value_len) { emit_run(" ", 1, _invert); emit_run(f.value, f.value_len, _invert); }
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
	const bool was_big = _big;
	newline();
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

		size_t n = ce - cs;
		if (n > 255) n = 255;
		if (_tlen + n > T_BYTES) { _table_overflowed = true; n = 0; }
		_cell_off[r][col] = _tlen;
		_cell_len[r][col] = (uint8_t)n;
		for (size_t k = 0; k < n; ++k) _tbuf[_tlen + k] = row[cs + k];
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
