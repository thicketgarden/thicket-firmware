// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Render Micron pages through the real driver and emit them for viewing.
//
// The picture comes out of VirtualPanel, which decodes the SPI byte stream
// rather than reading the framebuffer, so what is written to pages/ is what the
// panel would have been sent. A wrong address or bit order shows up as a wrong
// picture rather than as a passing test.
//
//   MICRON_PAGES="a.mu b.mu" pio test -e native -f test_page_render

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Micron.h"
#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "MicronRender.h"

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];
static char    src[256 * 1024];        // page source, held so links can point into it
static size_t  src_len = 0;

static bool load(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	src_len = fread(src, 1, sizeof(src) - 1, f);
	fclose(f);
	src[src_len] = 0;
	return true;
}

// One full pass: feed every line, then flush. Returns laid-out page height.
static uint16_t render(SharpLcd& lcd, PageRenderer& r, uint16_t scroll) {
	lcd.fill_white();
	micron::Parser p;
	p.reset();
	r.begin(scroll);
	size_t i = 0;
	while (i <= src_len) {
		size_t e = i;
		while (e < src_len && src[e] != '\n') ++e;
		if (e == i) r.blankLine();
		else        p.parseLine(src + i, e - i, r);
		if (e >= src_len) break;
		i = e + 1;
	}
	lcd.flush();
	return r.content_height();
}

static void slug_of(const char* path, char* out, size_t cap) {
	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;
	size_t n = 0;
	for (; base[n] && n + 1 < cap; ++n) {
		const char c = base[n];
		out[n] = (c == '.' || c == ' ') ? '_' : c;
	}
	out[n] = 0;
	char* dot = strstr(out, "_mu");
	if (dot) *dot = 0;
}

void test_render_pages(void) {
	const char* list = getenv("MICRON_PAGES");
	if (!list || !*list) { TEST_IGNORE_MESSAGE("set MICRON_PAGES"); return; }

	char buf[8192];
	strncpy(buf, list, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;

	int pages = 0, clipped = 0;
	for (char* tok = strtok(buf, " \n"); tok; tok = strtok(nullptr, " \n")) {
		if (!load(tok)) { printf("  MISSING %s\n", tok); continue; }

		// MICRON_LEADING and MICRON_FLAT let a comparison render be produced
		// without a rebuild, so two treatments can be judged side by side.
		const char* lead_s = getenv("MICRON_LEADING");
		const uint8_t lead = lead_s ? (uint8_t)atoi(lead_s) : 0;
		const bool stepped = getenv("MICRON_FLAT") == nullptr;
		const char* suffix = getenv("MICRON_SUFFIX");

		VirtualPanel panel;
		SharpLcd lcd(panel, fb);
		PageRenderer r(lcd, lead, stepped);

		const uint16_t h = render(lcd, r, 0);
		char slug[128]; slug_of(tok, slug, sizeof(slug));
		char out[256];
		snprintf(out, sizeof(out), "pages/%s%s.pbm", slug, suffix ? suffix : "");
		panel.write_pbm(out);

		const uint16_t screens = (uint16_t)((h + LCD_HEIGHT - 1) / LCD_HEIGHT);
		printf("  %-46s %5u px  %2u screen(s)  %2u link(s)%s\n",
		       slug, h, screens, r.link_count(),
		       r.links_overflowed() ? "  LINKS OVERFLOWED" : "");

		// A second screenful, where there is one, so scrolling is exercised
		// rather than assumed.
		if (h > LCD_HEIGHT) {
			render(lcd, r, LCD_HEIGHT);
			snprintf(out, sizeof(out), "pages/%s%s@2.pbm", slug, suffix ? suffix : "");
			panel.write_pbm(out);
		}
		if (panel.overflowed()) ++clipped;
		++pages;
	}
	printf("  %d page(s) rendered\n", pages);
	TEST_ASSERT_EQUAL_MESSAGE(0, clipped, "a page overflowed the wire buffer");
	TEST_ASSERT_TRUE(pages > 0);
}

// Layout invariants that do not need eyes.
void test_nothing_is_drawn_outside_the_panel(void) {
	const char* line = "`!A very long heading that runs past the right hand margin "
	                   "and keeps going well beyond sixty six characters to force a wrap`!";
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	PageRenderer r(lcd);
	lcd.fill_white();
	micron::Parser p; p.reset(); r.begin(0);
	p.parseLine(line, strlen(line), r);
	lcd.flush();
	// 120-odd characters cannot fit 66 cells, so it must have wrapped to a
	// second row. One row back would mean the text ran off the right margin.
	TEST_ASSERT_TRUE_MESSAGE(r.content_height() >= 2 * PageMetrics::LINE_H,
	                         "a line wider than the panel did not wrap");
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_render_pages);
	RUN_TEST(test_nothing_is_drawn_outside_the_panel);
	return UNITY_END();
}
