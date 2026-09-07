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
	_y = (uint16_t)(_y + line_h());
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
	if (!_row_open && _x < left_edge()) _x = left_edge();
	const uint16_t right = LCD_WIDTH - PageMetrics::MARGIN_X;

	size_t i = 0;
	while (i < n) {
		// One word, plus the spaces in front of it, measured before drawing so
		// the break lands between words and never inside a glyph.
		size_t sp = i;
		while (sp < n && is_space(t[sp])) ++sp;
		size_t w = sp;
		while (w < n && !is_space(t[w])) w += seq_len((uint8_t)t[w]);

		const uint16_t lead_w = (uint16_t)((sp - i) * PageMetrics::FONT_ADVANCE);
		const uint16_t word_w = (uint16_t)(cells(t + sp, w - sp) * PageMetrics::FONT_ADVANCE);

		// A word too long for a whole line is broken at the margin rather than
		// pushed off the panel. Long URLs are the case that matters.
		if (word_w > PageMetrics::content_w() - (left_edge() - PageMetrics::MARGIN_X)) {
			for (size_t k = i; k < w; ) {
				const uint8_t L = seq_len((uint8_t)t[k]);
				wrap_if_needed(PageMetrics::FONT_ADVANCE);
				if (row_visible(line_h())) {
					char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
					_lcd.draw_text(_x, screen_y(), g, !invert);
				}
				_x = (uint16_t)(_x + PageMetrics::FONT_ADVANCE);
				_row_open = true;
				k += L;
			}
			i = w;
			continue;
		}

		// Leading spaces are dropped at a wrap, never carried to column 0.
		if (_x + lead_w + word_w > right && _x > left_edge()) newline();
		else if (lead_w) {
			if (row_visible(line_h()) && invert)
				_lcd.fill_rect(_x, screen_y(), lead_w, PageMetrics::LINE_H, true);
			_x = (uint16_t)(_x + lead_w);
		}

		for (size_t k = sp; k < w; ) {
			const uint8_t L = seq_len((uint8_t)t[k]);
			if (row_visible(line_h())) {
				char g[5]; for (uint8_t b = 0; b < L; ++b) g[b] = t[k + b]; g[L] = 0;
				if (invert) _lcd.fill_rect(_x, screen_y(), PageMetrics::FONT_ADVANCE,
				                           PageMetrics::LINE_H, true);
				_lcd.draw_text(_x, screen_y(), g, !invert);
			}
			_x = (uint16_t)(_x + PageMetrics::FONT_ADVANCE);
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
		const uint16_t w = (uint16_t)(cells(t, n) * PageMetrics::FONT_ADVANCE);
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
	const bool head_bar = s.heading && (!_stepped || s.depth <= 1);
	const bool invert = head_bar || dark_bg;
	_head_rule = _stepped && s.heading && s.depth >= 2;
	_invert = invert;

	if (invert && !_row_open && row_visible(line_h())) {
		// Depth 1 takes the full width; depth 2 a band inset to its own indent,
		// so the levels read as different rather than as the same bar. A dark
		// background block always takes the full width: that is the page's own
		// colour, not a heading level.
		const bool inset = _stepped && head_bar && !dark_bg && s.depth >= 2;
		const uint16_t bx = inset ? left_edge() : PageMetrics::MARGIN_X;
		_lcd.fill_rect(bx, screen_y(), (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - bx),
		               line_h(), true);
	}
	emit_run(t, n, invert);

	// Underline stands in for the parser's underline AND for nothing else; the
	// font has no second weight to spend.
	if (s.underline && _row_open && row_visible(line_h()))
		_lcd.draw_hline(left_edge(), (uint16_t)(screen_y() + line_h() - 1),
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
	if (row_visible(line_h())) {
		const uint16_t x1 = _x;
		if (x1 > x0)
			_lcd.draw_hline(x0, (uint16_t)(y0 + line_h() - 2),
			                (uint16_t)(x1 - x0), !_invert);
		if (_link_count < MAX_LINKS) {
			LinkBox& b = _links[_link_count++];
			b.x = x0; b.y = y0;
			b.w = (uint16_t)(x1 > x0 ? x1 - x0 : 0);
			b.h = line_h();
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
	if (row_visible(line_h())) {
		const uint16_t y = (uint16_t)(screen_y() + line_h() / 2);
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
	if (_head_rule && _row_open && row_visible(line_h())) {
		// Depth 2 rules the full content width, deeper ones only their own
		// text, so the levels keep stepping down in weight.
		const uint16_t w = s.depth <= 2
			? (uint16_t)(LCD_WIDTH - PageMetrics::MARGIN_X - left_edge())
			: (uint16_t)(_x - left_edge());
		_lcd.draw_hline(left_edge(), (uint16_t)(screen_y() + line_h() - 1), w, true);
	}
	_head_rule = false;

	const bool was_heading = s.heading;
	newline();
	if (was_heading) _y = (uint16_t)(_y + PageMetrics::PARA_GAP);
	_invert = false;
}

void PageRenderer::onTableBegin(const micron::Table& t, const micron::Style& s) {
	_depth = s.depth;
	_in_table = true;
	_table_row = 0;
	_table_cols = 0;
	_table_align = t.align_set ? t.align : micron::Align::Left;
	for (uint8_t i = 0; i < 8; ++i) _col_w[i] = 0;
}

void PageRenderer::onTableRow(const char* row, size_t len, const micron::Style& s) {
	_depth = s.depth;
	// Markdown pipe rows. The separator row is consumed for alignment only.
	// Columns are sized evenly across the content width rather than measured,
	// because measuring needs the whole table and the whole table is exactly
	// what this renderer refuses to hold.
	size_t start = 0, end = len;
	while (start < end && (row[start] == '|' || is_space(row[start]))) ++start;
	while (end > start && (row[end - 1] == '|' || is_space(row[end - 1]))) --end;

	// Count columns once, on the first row.
	if (_table_cols == 0) {
		uint8_t c = 1;
		for (size_t i = start; i < end; ++i) if (row[i] == '|') ++c;
		_table_cols = c > 8 ? 8 : c;
		const uint8_t total = (uint8_t)(PageMetrics::cols()
		                                - _depth * (PageMetrics::INDENT_PX / PageMetrics::FONT_ADVANCE));
		for (uint8_t i = 0; i < _table_cols; ++i) _col_w[i] = (uint8_t)(total / _table_cols);
	}

	// A separator row (only -, : and spaces) sets nothing visible.
	bool sep = end > start;
	for (size_t i = start; i < end && sep; ++i)
		if (row[i] != '-' && row[i] != ':' && row[i] != '|' && !is_space(row[i])) sep = false;
	if (sep) { _table_row++; return; }

	const bool header = (_table_row == 0);
	if (header && row_visible(line_h()))
		_lcd.fill_rect(PageMetrics::MARGIN_X, screen_y(),
		               PageMetrics::content_w(), PageMetrics::LINE_H, true);

	uint8_t col = 0;
	size_t i = start;
	while (i <= end && col < _table_cols) {
		size_t cell = i;
		while (cell < end && row[cell] != '|') ++cell;
		size_t cs = i, ce = cell;
		while (cs < ce && is_space(row[cs])) ++cs;
		while (ce > cs && is_space(row[ce - 1])) --ce;

		// Cell content longer than its column is clipped to the column here.
		// Wrapping inside a cell needs a variable row height, which needs the
		// table held; noted in the log as the next thing to do.
		const uint16_t cx = (uint16_t)(left_edge() + col * _col_w[col] * PageMetrics::FONT_ADVANCE);
		size_t shown = ce - cs;
		if (cells(row + cs, shown) > _col_w[col]) {
			size_t k = cs; size_t c = 0;
			while (k < ce && c < (size_t)_col_w[col]) { k += seq_len((uint8_t)row[k]); ++c; }
			shown = k - cs;
		}
		_x = cx;
		if (row_visible(line_h())) emit_run(row + cs, shown, header);
		++col;
		i = cell + 1;
	}
	_table_row++;
	newline();
}

void PageRenderer::onTableEnd(const micron::Style& /*s*/) {
	_in_table = false;
	// A rule under the table, so it reads as a block rather than as stray text.
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
