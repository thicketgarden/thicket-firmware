// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Virtual panel: decode the wire stream back to an image and check it against
// what the driver was asked to draw. Also emits screens/*.pbm for viewing.

#include <unity.h>
#include <stdio.h>
#include "SharpLcd.h"
#include "VirtualPanel.h"

using namespace thicket;

static uint8_t fb[LCD_FB_BYTES];

// --- the panel must agree with the driver, decoded from bytes alone ---------

void test_decoded_image_matches_the_framebuffer(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();
	lcd.set_pixel(0, 0, true);
	lcd.set_pixel(399, 239, true);
	lcd.set_pixel(200, 120, true);
	lcd.flush();

	// The panel never saw the framebuffer. If the gate address or the bit
	// order were wrong these would land somewhere else.
	TEST_ASSERT_TRUE(panel.pixel(0, 0));
	TEST_ASSERT_TRUE(panel.pixel(399, 239));
	TEST_ASSERT_TRUE(panel.pixel(200, 120));
	TEST_ASSERT_FALSE(panel.pixel(1, 0));
	TEST_ASSERT_FALSE(panel.pixel(200, 121));
}

void test_every_pixel_round_trips(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();
	// A pattern that is asymmetric in x and y, so a transposed or reversed
	// address shows up rather than cancelling out.
	for (uint16_t y = 0; y < LCD_HEIGHT; ++y)
		for (uint16_t x = 0; x < LCD_WIDTH; ++x)
			if (((x * 7 + y * 13) & 31) < 9) lcd.set_pixel(x, y, true);
	lcd.flush();

	uint32_t mismatches = 0;
	for (uint16_t y = 0; y < LCD_HEIGHT; ++y)
		for (uint16_t x = 0; x < LCD_WIDTH; ++x)
			if (panel.pixel(x, y) != lcd.get_pixel(x, y)) ++mismatches;
	TEST_ASSERT_EQUAL(0, mismatches);
}

void test_only_dirty_lines_reach_the_panel(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();
	lcd.flush();
	panel.reset_counters();

	lcd.set_pixel(5, 42, true);
	lcd.flush();
	TEST_ASSERT_EQUAL(1, panel.lines_written());
	TEST_ASSERT_TRUE(panel.pixel(5, 42));
}

void test_clear_and_vcom_are_seen(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();
	lcd.set_pixel(10, 10, true);
	lcd.flush();
	TEST_ASSERT_TRUE(panel.pixel(10, 10));

	panel.reset_counters();
	lcd.toggle_vcom();
	TEST_ASSERT_EQUAL(1, panel.vcom_toggles());
	TEST_ASSERT_TRUE(panel.vcom());
	TEST_ASSERT_TRUE(panel.pixel(10, 10));   // vcom must not disturb pixels

	lcd.clear();
	TEST_ASSERT_EQUAL(1, panel.clears());
	TEST_ASSERT_FALSE(panel.pixel(10, 10));
}

// --- a real screen, rendered for viewing ------------------------------------

void test_render_message_screen(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();

	// Status bar
	lcd.fill_rect(0, 0, LCD_WIDTH, 14, true);
	lcd.draw_text(3, 1, "THICKET", false);
	lcd.draw_text(190, 1, "914.9 SF8", false);
	lcd.draw_text(328, 1, "BATT 82%", false);

	// Conversation
	lcd.draw_text(3, 17, "0795eea0  T-Deck", true);
	lcd.draw_hline(0, 32, LCD_WIDTH, true);

	lcd.draw_text(3,  36, "them  hi", true);
	lcd.draw_text(3,  50, "us    #0 up73s r-46 s+13.5", true);
	lcd.draw_text(3,  64, "them  hi 2", true);
	lcd.draw_text(3,  78, "us    #1 up697s r-73 s+12.8", true);

	// Telegram tape
	lcd.draw_hline(0, 194, LCD_WIDTH, true);
	lcd.draw_text(3, 199, "QUEUED", true);
	lcd.draw_text(60, 199, "ON AIR", true);
	lcd.draw_text(120, 199, "RELAY", true);
	lcd.fill_rect(176, 196, 62, 16, true);
	lcd.draw_text(178, 199, "DELIVD", false);

	// Compose line
	lcd.draw_hline(0, 218, LCD_WIDTH, true);
	lcd.draw_text(3, 224, "> on my way_", true);

	lcd.flush();

	TEST_ASSERT_TRUE(panel.write_pbm("screens/message.pbm", 2));
	TEST_ASSERT_TRUE(panel.lines_written() > 100);
}

void test_render_boot_screen(void) {
	VirtualPanel panel;
	SharpLcd lcd(panel, fb);
	lcd.fill_white();

	lcd.draw_text(164, 40, "THICKET", true);
	lcd.draw_hline(140, 52, 120, true);
	lcd.draw_text(146, 70, "Reticulum handheld", true);

	lcd.draw_text(60, 110, "radio", true);
	lcd.draw_text(200, 110, "914.875 MHz SF8", true);
	lcd.draw_text(60, 124, "identity", true);
	lcd.draw_text(200, 124, "7ebdb49255c0bc6e", true);
	lcd.draw_text(60, 138, "store", true);
	lcd.draw_text(200, 138, "encrypted, 0 msgs", true);
	lcd.draw_text(60, 152, "peers", true);
	lcd.draw_text(200, 152, "1 known", true);

	lcd.fill_rect(164, 188, 72, 18, true);
	lcd.draw_text(167, 191, "LISTENING", false);

	lcd.flush();
	TEST_ASSERT_TRUE(panel.write_pbm("screens/boot.pbm", 2));
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_decoded_image_matches_the_framebuffer);
	RUN_TEST(test_every_pixel_round_trips);
	RUN_TEST(test_only_dirty_lines_reach_the_panel);
	RUN_TEST(test_clear_and_vcom_are_seen);
	RUN_TEST(test_render_message_screen);
	RUN_TEST(test_render_boot_screen);
	return UNITY_END();
}
