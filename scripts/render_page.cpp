// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Host renderer: page in, PBM out, through the real driver and the real font.
//
// Built with a bare compiler rather than through PlatformIO, because the render
// loop is look-fix-look and a 75-second test cycle is not a loop. The pixels
// come from VirtualPanel, which decodes the SPI stream, so this is the same
// picture the panel would be sent.
//
//   scripts/pages.sh   builds and drives this

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Micron.h"
#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "MicronRender.h"

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];
static std::string src;

static uint16_t render(SharpLcd& lcd, PageRenderer& r, uint16_t scroll) {
	lcd.fill_white();
	micron::Parser p; p.reset(); r.begin(scroll);
	size_t i = 0;
	while (i <= src.size()) {
		size_t e = src.find('\n', i);
		if (e == std::string::npos) e = src.size();
		if (e == i) r.blankLine();
		else        p.parseLine(src.data() + i, e - i, r);
		if (e >= src.size()) break;
		i = e + 1;
	}
	lcd.flush();
	return r.content_height();
}

int main(int argc, char** argv) {
	if (argc < 3) { std::fprintf(stderr, "usage: render_page <page.mu> <out-prefix> [leading] [flat|step] [2x]\n"); return 2; }
	const uint8_t lead = argc > 3 ? (uint8_t)atoi(argv[3]) : 0;
	const bool stepped = !(argc > 4 && std::strcmp(argv[4], "flat") == 0);
	const bool big = argc > 5 && std::strcmp(argv[5], "2x") == 0;

	FILE* f = std::fopen(argv[1], "rb");
	if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	char buf[65536]; size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
	std::fclose(f);

	VirtualPanel panel; SharpLcd lcd(panel, fb);
	PageRenderer r(lcd, lead, stepped, big);

	const uint16_t h = render(lcd, r, 0);
	char out[512];
	std::snprintf(out, sizeof out, "%s.pbm", argv[2]);
	panel.write_pbm(out);

	int screens = 1;
	for (uint16_t s = LCD_HEIGHT; s < h && screens < 6; s = (uint16_t)(s + LCD_HEIGHT)) {
		render(lcd, r, s);
		std::snprintf(out, sizeof out, "%s@%d.pbm", argv[2], ++screens);
		panel.write_pbm(out);
	}
	std::printf("%-44s %5u px  %d screen(s)  %2u link(s)%s%s\n",
	            argv[2], h, screens, r.link_count(),
	            r.links_overflowed() ? "  LINKS-OVERFLOW" : "",
	            panel.overflowed() ? "  WIRE-OVERFLOW" : "");
	return 0;
}
