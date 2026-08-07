// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Host tests for the Sharp LS027B7DH01 driver: `pio test -e native`.
// Wire-format expectations come from spec LCP-2110015A sections 6-5 and 6-6.

#include <unity.h>
#include <vector>
#include <string.h>
#include "SharpLcd.h"

using namespace thicket;

// Records the exact byte stream and the SCS transitions around it.
class FakeBus : public SharpLcdBus {
public:
	std::vector<uint8_t> bytes;
	std::vector<bool>    selects;
	bool selected = false;

	void select(bool on) override { selected = on; selects.push_back(on); }
	void write(const uint8_t* data, size_t len) override {
		TEST_ASSERT_TRUE_MESSAGE(selected, "wrote while SCS was low");
		for (size_t i = 0; i < len; ++i) bytes.push_back(data[i]);
	}
	void reset() { bytes.clear(); selects.clear(); }
};

static uint8_t fb[LCD_FB_BYTES];

// --- wire encoding --------------------------------------------------------

void test_gate_addresses_match_the_datasheet_table(void) {
	// LCP-2110015A 6-6 gives AG0..AG7 per line. AG0 is sent first and is the
	// low bit, so the byte on an MSB-first bus is the reversed line number.
	// L1 = H L L L L L L L, L238 = L H H H L H H H, L240 = L L L L H H H H.
	TEST_ASSERT_EQUAL_HEX8(0x80, SharpLcd::line_address(1));
	TEST_ASSERT_EQUAL_HEX8(0x40, SharpLcd::line_address(2));
	TEST_ASSERT_EQUAL_HEX8(0xC0, SharpLcd::line_address(3));
	TEST_ASSERT_EQUAL_HEX8(0x77, SharpLcd::line_address(238));
	TEST_ASSERT_EQUAL_HEX8(0xF7, SharpLcd::line_address(239));
	TEST_ASSERT_EQUAL_HEX8(0x0F, SharpLcd::line_address(240));
}

void test_reverse_bits(void) {
	TEST_ASSERT_EQUAL_HEX8(0x00, SharpLcd::reverse_bits(0x00));
	TEST_ASSERT_EQUAL_HEX8(0xFF, SharpLcd::reverse_bits(0xFF));
	TEST_ASSERT_EQUAL_HEX8(0x80, SharpLcd::reverse_bits(0x01));
	TEST_ASSERT_EQUAL_HEX8(0x01, SharpLcd::reverse_bits(0x80));
	TEST_ASSERT_EQUAL_HEX8(0xA5, SharpLcd::reverse_bits(0xA5));
}

// --- pixels ---------------------------------------------------------------

void test_pixel_bit_order_is_leftmost_first(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.set_pixel(0, 0, true);
	// x=0 is the leftmost pixel and must be bit 7, since bit 7 leaves an
	// MSB-first bus first and the panel takes D1 first.
	TEST_ASSERT_EQUAL_HEX8(0x7F, fb[0]);
	lcd.set_pixel(7, 0, true);
	TEST_ASSERT_EQUAL_HEX8(0x7E, fb[0]);
}

void test_pixel_round_trip(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	TEST_ASSERT_FALSE(lcd.get_pixel(399, 239));
	lcd.set_pixel(399, 239, true);
	TEST_ASSERT_TRUE(lcd.get_pixel(399, 239));
	lcd.set_pixel(399, 239, false);
	TEST_ASSERT_FALSE(lcd.get_pixel(399, 239));
}

void test_out_of_range_pixels_are_ignored_not_wrapped(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();                       // clear dirty state
	lcd.set_pixel(LCD_WIDTH, 0, true);
	lcd.set_pixel(0, LCD_HEIGHT, true);
	// A wrapped write would corrupt another line and dirty it.
	TEST_ASSERT_EQUAL(0, lcd.dirty_lines());
}

// --- dirty tracking -------------------------------------------------------

void test_only_changed_lines_are_dirty(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	TEST_ASSERT_EQUAL(0, lcd.dirty_lines());
	lcd.set_pixel(10, 5, true);
	lcd.set_pixel(11, 5, true);
	lcd.set_pixel(10, 200, true);
	TEST_ASSERT_EQUAL(2, lcd.dirty_lines());
}

void test_writing_the_same_value_does_not_dirty(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	lcd.set_pixel(10, 5, false);       // already white
	TEST_ASSERT_EQUAL(0, lcd.dirty_lines());
}

void test_flush_sends_nothing_when_clean(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();
	TEST_ASSERT_EQUAL(0, lcd.flush());
	TEST_ASSERT_EQUAL(0, bus.bytes.size());
	TEST_ASSERT_EQUAL(0, bus.selects.size());
}

// --- transaction framing ---------------------------------------------------

void test_flush_frame_for_one_line(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();

	lcd.set_pixel(0, 2, true);         // line index 2 -> gate line 3
	TEST_ASSERT_EQUAL(1, lcd.flush());

	// mode + address + 50 data + gap + trailer
	TEST_ASSERT_EQUAL(1 + 1 + LCD_LINE_BYTES + 1 + 1, bus.bytes.size());
	TEST_ASSERT_EQUAL_HEX8(LCD_M0_WRITE, bus.bytes[0]);
	TEST_ASSERT_EQUAL_HEX8(0xC0, bus.bytes[1]);   // line 3, LSB-first
	TEST_ASSERT_EQUAL_HEX8(0x7F, bus.bytes[2]);          // pixel 0 black
	TEST_ASSERT_EQUAL_HEX8(0x00, bus.bytes.back());
}

void test_scs_is_high_for_the_whole_transaction(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();
	lcd.set_pixel(0, 0, true);
	lcd.flush();
	// Active HIGH: rises before data, falls after. FakeBus::write also asserts
	// that nothing is written while it is low.
	TEST_ASSERT_EQUAL(2, bus.selects.size());
	TEST_ASSERT_TRUE(bus.selects[0]);
	TEST_ASSERT_FALSE(bus.selects[1]);
}

void test_multiple_lines_share_one_transaction(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();

	lcd.set_pixel(0, 0, true);
	lcd.set_pixel(0, 100, true);
	lcd.set_pixel(0, 239, true);
	TEST_ASSERT_EQUAL(3, lcd.flush());

	TEST_ASSERT_EQUAL(2, bus.selects.size());   // one transaction, not three
	TEST_ASSERT_EQUAL(1 + 3 * (1 + LCD_LINE_BYTES + 1) + 1, bus.bytes.size());
	TEST_ASSERT_EQUAL_HEX8(0x80, bus.bytes[1]);                       // line 1
	TEST_ASSERT_EQUAL_HEX8(0xA6, bus.bytes[1 + (1 + LCD_LINE_BYTES + 1)]);
	TEST_ASSERT_EQUAL_HEX8(0x0F, bus.bytes[1 + 2 * (1 + LCD_LINE_BYTES + 1)]);
}

void test_lines_are_sent_in_ascending_order(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();
	lcd.set_pixel(0, 200, true);
	lcd.set_pixel(0, 3, true);
	lcd.flush();
	TEST_ASSERT_EQUAL_HEX8(0x20, bus.bytes[1]);                       // line 4
	TEST_ASSERT_EQUAL_HEX8(0x93, bus.bytes[1 + (1 + LCD_LINE_BYTES + 1)]);
}

void test_flush_clears_dirty_state(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.set_pixel(0, 0, true);
	lcd.flush();
	TEST_ASSERT_EQUAL(0, lcd.dirty_lines());
}

// --- VCOM and clear --------------------------------------------------------

void test_vcom_alternates_and_carries_into_writes(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();

	TEST_ASSERT_FALSE(lcd.vcom());
	bus.reset();
	lcd.toggle_vcom();
	TEST_ASSERT_TRUE(lcd.vcom());
	TEST_ASSERT_EQUAL(2, bus.bytes.size());          // mode + dummy, no data
	TEST_ASSERT_EQUAL_HEX8(LCD_M1_VCOM, bus.bytes[0]);

	// The flag must ride along on a data update too, or a screen that is being
	// written never inverts.
	bus.reset();
	lcd.set_pixel(0, 0, true);
	lcd.flush();
	TEST_ASSERT_EQUAL_HEX8(LCD_M0_WRITE | LCD_M1_VCOM, bus.bytes[0]);

	bus.reset();
	lcd.toggle_vcom();
	TEST_ASSERT_FALSE(lcd.vcom());
	TEST_ASSERT_EQUAL_HEX8(0x00, bus.bytes[0]);
}

void test_toggle_vcom_sends_no_pixel_data(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.flush();
	bus.reset();
	lcd.toggle_vcom();
	// Two bytes only. Anything longer means a display-mode command is dragging
	// the framebuffer along with it.
	TEST_ASSERT_EQUAL(2, bus.bytes.size());
}

void test_clear_uses_the_panel_command_not_a_full_transfer(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.set_pixel(0, 0, true);
	bus.reset();
	lcd.clear();

	TEST_ASSERT_EQUAL(2, bus.bytes.size());
	TEST_ASSERT_EQUAL_HEX8(LCD_M2_CLEAR, bus.bytes[0]);
	TEST_ASSERT_TRUE(lcd.get_pixel(0, 0) == false);   // buffer went white too
	TEST_ASSERT_EQUAL(0, lcd.dirty_lines());          // and agrees with the panel
}

void test_clear_carries_vcom(void) {
	FakeBus bus; SharpLcd lcd(bus, fb);
	lcd.fill_white();
	lcd.toggle_vcom();
	bus.reset();
	lcd.clear();
	TEST_ASSERT_EQUAL_HEX8(LCD_M2_CLEAR | LCD_M1_VCOM, bus.bytes[0]);
}

// --- budget ----------------------------------------------------------------

void test_framebuffer_size_matches_the_ram_budget(void) {
	// 12,000 B is what the RAM budget reserves. A change here moves the
	// largest single allocation in the device.
	TEST_ASSERT_EQUAL(12000, LCD_FB_BYTES);
	TEST_ASSERT_EQUAL(50, LCD_LINE_BYTES);
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_gate_addresses_match_the_datasheet_table);
	RUN_TEST(test_reverse_bits);
	RUN_TEST(test_pixel_bit_order_is_leftmost_first);
	RUN_TEST(test_pixel_round_trip);
	RUN_TEST(test_out_of_range_pixels_are_ignored_not_wrapped);
	RUN_TEST(test_only_changed_lines_are_dirty);
	RUN_TEST(test_writing_the_same_value_does_not_dirty);
	RUN_TEST(test_flush_sends_nothing_when_clean);
	RUN_TEST(test_flush_frame_for_one_line);
	RUN_TEST(test_scs_is_high_for_the_whole_transaction);
	RUN_TEST(test_multiple_lines_share_one_transaction);
	RUN_TEST(test_lines_are_sent_in_ascending_order);
	RUN_TEST(test_flush_clears_dirty_state);
	RUN_TEST(test_vcom_alternates_and_carries_into_writes);
	RUN_TEST(test_toggle_vcom_sends_no_pixel_data);
	RUN_TEST(test_clear_uses_the_panel_command_not_a_full_transfer);
	RUN_TEST(test_clear_carries_vcom);
	RUN_TEST(test_framebuffer_size_matches_the_ram_budget);
	return UNITY_END();
}
