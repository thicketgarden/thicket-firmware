// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interactive UI host. Reads key events on stdin, runs them through the real
// InputLayer and SharpLcd, renders via VirtualPanel, writes a PBM per frame.
//
//   ui_host OUT.pbm
//   stdin: one command per line
//     k <char>   ordinary key
//     m shift|alt|sym   one-shot modifier
//     enter | back
//     q          quit

#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "InputLayer.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

// The dev keymap. Real layouts are data; this only has to be typeable.
enum : uint8_t { K_BASE = KEY_FIRST_ORDINARY };

static char keymap(uint8_t code, uint8_t mods, void*) {
	const char c = (char)(code - K_BASE);
	if (mods & MOD_SHIFT && c >= 'a' && c <= 'z') return (char)(c - 32);
	if (mods & MOD_SYM) {
		switch (c) {
			case 'q': return '1'; case 'w': return '2'; case 'e': return '3';
			case 'r': return '4'; case 't': return '5'; case 'y': return '6';
			case 'u': return '7'; case 'i': return '8'; case 'o': return '9';
			case 'p': return '0'; case 'a': return '@'; case 's': return '#';
			case 'd': return '.'; case 'f': return ','; case 'g': return '?';
			default:  return c;
		}
	}
	return c;
}

struct Msg { bool ours; std::string text; std::string meta; };

static std::vector<Msg> log_;
static std::string compose;
static uint8_t pending_mods = 0;
static uint32_t refused = 0;

static void draw(SharpLcd& lcd) {
	lcd.fill_white();

	lcd.fill_rect(0, 0, LCD_WIDTH, 14, true);
	lcd.draw_text(3, 1, "THICKET", false);
	lcd.draw_text(190, 1, "914.9 SF8", false);
	lcd.draw_text(328, 1, "BATT 82%", false);

	lcd.draw_text(3, 17, "0795eea0  T-Deck", true);
	lcd.draw_hline(0, 32, LCD_WIDTH, true);

	// Newest at the bottom, 11 rows of history at a 14-pixel pitch.
	const int rows = 11;
	int first = (int)log_.size() - rows;
	if (first < 0) first = 0;
	int y = 36;
	for (size_t i = (size_t)first; i < log_.size(); ++i) {
		lcd.draw_text(3, (uint16_t)y, log_[i].ours ? "us  " : "them", true);
		lcd.draw_text(36, (uint16_t)y, log_[i].text.c_str(), true);
		if (!log_[i].meta.empty())
			lcd.draw_text(340, (uint16_t)y, log_[i].meta.c_str(), true);
		y += 14;
	}

	lcd.draw_hline(0, 194, LCD_WIDTH, true);
	lcd.draw_text(3, 199, "QUEUED", true);
	lcd.draw_text(60, 199, "ON AIR", true);
	lcd.draw_text(120, 199, "RELAY", true);
	if (!log_.empty() && log_.back().ours) {
		lcd.fill_rect(176, 196, 62, 16, true);
		lcd.draw_text(178, 199, "DELIVD", false);
	} else {
		lcd.draw_text(178, 199, "DELIVD", true);
	}

	// Modifier state has to be visible or one-shot modifiers are unusable.
	char mods[24] = "";
	if (pending_mods & MOD_SHIFT) strcat(mods, "SHIFT ");
	if (pending_mods & MOD_ALT)   strcat(mods, "ALT ");
	if (pending_mods & MOD_SYM)   strcat(mods, "SYM ");
	if (mods[0]) lcd.draw_text(250, 199, mods, true);
	if (refused) {
		char r[32];
		snprintf(r, sizeof(r), "GHOST %u", refused);
		lcd.draw_text(330, 199, r, true);
	}

	lcd.draw_hline(0, 218, LCD_WIDTH, true);
	std::string line = "> " + compose + "_";
	lcd.draw_text(3, 224, line.c_str(), true);

	lcd.flush();
}

int main(int argc, char** argv) {
	if (argc < 2) { fprintf(stderr, "usage: ui_host OUT.pbm\n"); return 2; }
	const char* out = argv[1];

	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	InputLayer input(keymap, nullptr);

	log_.push_back({false, "hi", ""});
	log_.push_back({true,  "#0 up73s r-46", "ok"});
	log_.push_back({false, "hi 2", ""});

	draw(lcd);
	panel.write_pbm(out, 2);
	printf("ready\n");
	fflush(stdout);

	char line[256];
	while (fgets(line, sizeof(line), stdin)) {
		char* nl = strchr(line, '\n'); if (nl) *nl = 0;
		if (line[0] == 'q') break;

		if (line[0] == 'k' && line[1] == ' ' && line[2]) {
			const uint8_t code = (uint8_t)(K_BASE + (uint8_t)line[2]);
			Event e = input.key_down(code);
			input.key_up(code);
			if (e.type == Event::CHAR) compose.push_back(e.ch);
		}
		else if (line[0] == 'm' && line[1] == ' ') {
			uint8_t code = KEY_SHIFT;
			if (!strcmp(line + 2, "alt")) code = KEY_ALT;
			else if (!strcmp(line + 2, "sym")) code = KEY_SYM;
			input.key_down(code);
			input.key_up(code);
		}
		else if (!strcmp(line, "back")) {
			if (!compose.empty()) compose.pop_back();
		}
		else if (!strcmp(line, "enter")) {
			if (!compose.empty()) {
				log_.push_back({true, compose, "ok"});
				compose.clear();
			}
		}

		pending_mods = input.pending_mods();
		refused = input.refused();

		draw(lcd);
		panel.write_pbm(out, 2);
		printf("ok %zu\n", log_.size());
		fflush(stdout);
	}
	return 0;
}
