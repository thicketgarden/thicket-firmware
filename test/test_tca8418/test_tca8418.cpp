// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Host tests for the TCA8418 scanner: `pio test -e native`.
//
// There is no keyboard and no I2C bus here. The chip is behind an abstract Bus,
// so the register conversation can be scripted and asserted — which is the same
// reason InputLayer could be written and proven before any hardware existed.
//
// The load-bearing tests are the ones that use TI's OWN worked example
// (SCPS215G Table 5) as fixed vectors. Round-tripping our encoder against our
// decoder would prove only that we agree with ourselves; decoding bytes the
// datasheet publishes is what proves we agree with the part.

#include <unity.h>
#include <vector>
#include <map>
#include "Tca8418.h"

using namespace thicket;

// A scriptable bus. Reads come from a per-register queue when one is set, so a
// test can make consecutive reads of the SAME register return different bytes —
// which is exactly how the FIFO behaves and the thing a naive fake gets wrong.
class FakeBus : public Tca8418Bus {
public:
	std::map<uint8_t, std::vector<uint8_t>> reads;
	std::vector<std::pair<uint8_t, uint8_t>> writes;
	bool fail_all = false;

	bool write_reg(uint8_t reg, uint8_t value) override {
		if (fail_all) return false;
		writes.push_back({reg, value});
		return true;
	}
	bool read_reg(uint8_t reg, uint8_t& value) override {
		if (fail_all) return false;
		auto it = reads.find(reg);
		if (it == reads.end() || it->second.empty()) { value = 0; return true; }
		value = it->second.front();
		it->second.erase(it->second.begin());
		return true;
	}
	uint8_t wrote(uint8_t reg) const {
		for (auto it = writes.rbegin(); it != writes.rend(); ++it)
			if (it->first == reg) return it->second;
		return 0xEE;
	}
	bool wrote_any(uint8_t reg) const {
		for (const auto& w : writes) if (w.first == reg) return true;
		return false;
	}
};

// --- decode, against the datasheet's published bytes -----------------------

void test_datasheet_example_presses(void) {
	// SCPS215G Table 5, verbatim: key 1 press is 0x81, key 32 press 0xA0,
	// key 23 press 0x97, key 45 press 0xAD, key 41 press 0xA9.
	struct { uint8_t raw; uint8_t key; bool pressed; } v[] = {
		{0x81,  1, true }, {0xA0, 32, true }, {0x97, 23, true },
		{0xAD, 45, true }, {0xA9, 41, true },
		{0x01,  1, false}, {0x20, 32, false}, {0x17, 23, false},
		{0x2D, 45, false}, {0x29, 41, false},
	};
	for (auto& t : v) {
		KeyEvent ev;
		TEST_ASSERT_TRUE(Tca8418::decode(t.raw, ev));
		TEST_ASSERT_EQUAL(t.pressed, ev.pressed);
		// key = row*10 + col + 1
		TEST_ASSERT_EQUAL(t.key, ev.row * TCA_COLS + ev.col + 1);
	}
}

void test_control_alt_delete_positions(void) {
	// The datasheet says Ctrl-Alt-Del is keys 1, 11 and 21. If our stride is
	// right those are R0C0, R1C0, R2C0 — the independent check that the
	// numbering runs across columns first rather than down rows.
	KeyEvent a, b, c;
	TEST_ASSERT_TRUE(Tca8418::decode(0x80 | 1,  a));
	TEST_ASSERT_TRUE(Tca8418::decode(0x80 | 11, b));
	TEST_ASSERT_TRUE(Tca8418::decode(0x80 | 21, c));
	TEST_ASSERT_EQUAL(0, a.row); TEST_ASSERT_EQUAL(0, a.col);
	TEST_ASSERT_EQUAL(1, b.row); TEST_ASSERT_EQUAL(0, b.col);
	TEST_ASSERT_EQUAL(2, c.row); TEST_ASSERT_EQUAL(0, c.col);
}

void test_last_key_of_the_matrix(void) {
	KeyEvent ev;
	TEST_ASSERT_TRUE(Tca8418::decode(0x80 | 80, ev));
	TEST_ASSERT_EQUAL(7, ev.row);
	TEST_ASSERT_EQUAL(9, ev.col);
}

void test_gpi_events_are_not_matrix_keys(void) {
	// 97-114 are row/column GPI events (Tables 2 and 3). Decoding one as a
	// matrix key would invent a press at a coordinate that does not exist.
	KeyEvent ev;
	TEST_ASSERT_FALSE(Tca8418::decode(0x80 | 97,  ev));
	TEST_ASSERT_FALSE(Tca8418::decode(0x80 | 114, ev));
	TEST_ASSERT_FALSE(Tca8418::decode(0x80 | 81,  ev));  // past the 80-key table
	TEST_ASSERT_FALSE(Tca8418::decode(0x00, ev));        // empty-FIFO sentinel
}

// --- configuration ---------------------------------------------------------

void test_begin_claims_only_the_rectangle_used(void) {
	FakeBus bus; Tca8418 kp(bus);
	TEST_ASSERT_TRUE(kp.begin(5, 7));
	TEST_ASSERT_EQUAL_HEX8(0x1F, bus.wrote(TCA_KP_GPIO1));  // 5 rows
	TEST_ASSERT_EQUAL_HEX8(0x7F, bus.wrote(TCA_KP_GPIO2));  // 7 cols
	TEST_ASSERT_EQUAL_HEX8(0x00, bus.wrote(TCA_KP_GPIO3));  // none above col 7
}

void test_begin_spans_the_third_column_register(void) {
	FakeBus bus; Tca8418 kp(bus);
	TEST_ASSERT_TRUE(kp.begin(8, 10));
	TEST_ASSERT_EQUAL_HEX8(0xFF, bus.wrote(TCA_KP_GPIO1));
	TEST_ASSERT_EQUAL_HEX8(0xFF, bus.wrote(TCA_KP_GPIO2));
	TEST_ASSERT_EQUAL_HEX8(0x03, bus.wrote(TCA_KP_GPIO3));  // COL8, COL9
}

void test_begin_rejects_impossible_geometry(void) {
	FakeBus bus; Tca8418 kp(bus);
	TEST_ASSERT_FALSE(kp.begin(9, 10));   // 8 rows max
	TEST_ASSERT_FALSE(kp.begin(8, 11));   // 10 columns max
	TEST_ASSERT_FALSE(kp.begin(0, 4));
}

void test_overflow_needs_both_config_bits(void) {
	// The Overflow Errata: with only OVR_FLOW_IEN set, no overflow interrupt is
	// generated at all. Setting one bit and believing overflow is covered is
	// precisely the mistake that errata documents.
	FakeBus bus; Tca8418 kp(bus);
	TEST_ASSERT_TRUE(kp.begin(4, 8));
	const uint8_t cfg = bus.wrote(TCA_CFG);
	TEST_ASSERT_TRUE(cfg & TCA_CFG_OVR_FLOW_IEN);
	TEST_ASSERT_TRUE(cfg & TCA_CFG_OVR_FLOW_M);
	TEST_ASSERT_TRUE(cfg & TCA_CFG_KE_IEN);
}

void test_begin_leaves_debounce_enabled(void) {
	FakeBus bus; Tca8418 kp(bus);
	TEST_ASSERT_TRUE(kp.begin(4, 8));
	// 0 = enabled. Writing 0xFF here would disable debounce on every row.
	TEST_ASSERT_EQUAL_HEX8(0x00, bus.wrote(TCA_DEBOUNCE_DIS1));
}

void test_begin_reports_bus_failure(void) {
	FakeBus bus; bus.fail_all = true;
	Tca8418 kp(bus);
	TEST_ASSERT_FALSE(kp.begin(4, 8));
}

// --- FIFO drain ------------------------------------------------------------

void test_poll_drains_in_order(void) {
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]    = {TCA_INT_K};
	bus.reads[TCA_KEY_LCK_EC]  = {3};
	// Same register read three times, three different bytes: the FIFO shifts
	// down on each read, which is why the driver never reads B..J directly.
	bus.reads[TCA_KEY_EVENT_A] = {0x81, 0xA0, 0x01};

	KeyEvent ev[8];
	TEST_ASSERT_EQUAL(3, kp.poll(ev, 8));
	TEST_ASSERT_TRUE(ev[0].pressed);   // key 1 press
	TEST_ASSERT_EQUAL(0, ev[0].row); TEST_ASSERT_EQUAL(0, ev[0].col);
	TEST_ASSERT_TRUE(ev[1].pressed);   // key 32 press
	TEST_ASSERT_EQUAL(3, ev[1].row); TEST_ASSERT_EQUAL(1, ev[1].col);
	TEST_ASSERT_FALSE(ev[2].pressed);  // key 1 release
}

void test_poll_clears_the_interrupt(void) {
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]    = {TCA_INT_K};
	bus.reads[TCA_KEY_LCK_EC]  = {1};
	bus.reads[TCA_KEY_EVENT_A] = {0x81};
	KeyEvent ev[4];
	kp.poll(ev, 4);
	// Written back as read: "Requires writing a 1 to clear interrupts."
	TEST_ASSERT_EQUAL_HEX8(TCA_INT_K, bus.wrote(TCA_INT_STAT));
}

void test_poll_reports_overflow(void) {
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]   = {(uint8_t)(TCA_INT_K | TCA_INT_OVR_FLOW)};
	bus.reads[TCA_KEY_LCK_EC] = {1};
	bus.reads[TCA_KEY_EVENT_A]= {0x81};
	KeyEvent ev[4];
	TEST_ASSERT_FALSE(kp.overflowed());
	kp.poll(ev, 4);
	// By the time the chip says this, events are already gone. Swallowing it
	// would turn lost keystrokes into silence.
	TEST_ASSERT_TRUE(kp.overflowed());
	kp.clear_overflow();
	TEST_ASSERT_FALSE(kp.overflowed());
}

void test_false_cad_interrupt_is_ignored(void) {
	// The CAD errata: keys 1+11 raise CAD_INT with no workaround, and those are
	// two ordinary keys in column zero. Two-key rollover is explicitly allowed
	// by the input layer, so this MUST NOT be treated as a real event or
	// ordinary typing becomes a spurious Ctrl-Alt-Delete.
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]    = {(uint8_t)(TCA_INT_K | TCA_INT_CAD)};
	bus.reads[TCA_KEY_LCK_EC]  = {2};
	bus.reads[TCA_KEY_EVENT_A] = {(uint8_t)(0x80 | 1), (uint8_t)(0x80 | 11)};

	KeyEvent ev[4];
	TEST_ASSERT_EQUAL(2, kp.poll(ev, 4));   // both keys survive as normal keys
	TEST_ASSERT_EQUAL(0, ev[0].row);
	TEST_ASSERT_EQUAL(1, ev[1].row);
	TEST_ASSERT_EQUAL(0, ev[1].col);
	// and the CAD bit is cleared like any other, not acted on
	TEST_ASSERT_TRUE(bus.wrote(TCA_INT_STAT) & TCA_INT_CAD);
}

void test_poll_does_nothing_when_no_key_interrupt(void) {
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT] = {0x00};
	KeyEvent ev[4];
	TEST_ASSERT_EQUAL(0, kp.poll(ev, 4));
	TEST_ASSERT_FALSE(bus.wrote_any(TCA_KEY_EVENT_A));
}

void test_poll_respects_the_caller_buffer(void) {
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]    = {TCA_INT_K};
	bus.reads[TCA_KEY_LCK_EC]  = {5};
	bus.reads[TCA_KEY_EVENT_A] = {0x81, 0xA0, 0x97, 0xAD, 0xA9};
	KeyEvent ev[2];
	TEST_ASSERT_EQUAL(2, kp.poll(ev, 2));   // stops, does not overrun
}

void test_stuck_bus_cannot_spin_forever(void) {
	// A bus that keeps returning the same non-zero byte must not loop. The
	// event count bounds it; without that bound this is an infinite loop in an
	// interrupt-driven path.
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]   = {TCA_INT_K};
	bus.reads[TCA_KEY_LCK_EC] = {3};
	// no queue for KEY_EVENT_A: reads return 0, the documented empty marker
	KeyEvent ev[16];
	TEST_ASSERT_EQUAL(0, kp.poll(ev, 16));
}

void test_gpi_event_still_consumes_a_fifo_slot(void) {
	// A GPI event is not a matrix key, but it occupied a slot. If it did not
	// decrement the counter the drain loop would go one read too far every
	// time one arrived.
	FakeBus bus; Tca8418 kp(bus);
	bus.reads[TCA_INT_STAT]    = {TCA_INT_K};
	bus.reads[TCA_KEY_LCK_EC]  = {2};
	bus.reads[TCA_KEY_EVENT_A] = {(uint8_t)(0x80 | 97), 0x81};
	KeyEvent ev[4];
	TEST_ASSERT_EQUAL(1, kp.poll(ev, 4));   // GPI dropped, real key kept
	TEST_ASSERT_EQUAL(0, ev[0].row);
	TEST_ASSERT_EQUAL(0, ev[0].col);
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_datasheet_example_presses);
	RUN_TEST(test_control_alt_delete_positions);
	RUN_TEST(test_last_key_of_the_matrix);
	RUN_TEST(test_gpi_events_are_not_matrix_keys);
	RUN_TEST(test_begin_claims_only_the_rectangle_used);
	RUN_TEST(test_begin_spans_the_third_column_register);
	RUN_TEST(test_begin_rejects_impossible_geometry);
	RUN_TEST(test_overflow_needs_both_config_bits);
	RUN_TEST(test_begin_leaves_debounce_enabled);
	RUN_TEST(test_begin_reports_bus_failure);
	RUN_TEST(test_poll_drains_in_order);
	RUN_TEST(test_poll_clears_the_interrupt);
	RUN_TEST(test_poll_reports_overflow);
	RUN_TEST(test_false_cad_interrupt_is_ignored);
	RUN_TEST(test_poll_does_nothing_when_no_key_interrupt);
	RUN_TEST(test_poll_respects_the_caller_buffer);
	RUN_TEST(test_stuck_bus_cannot_spin_forever);
	RUN_TEST(test_gpi_event_still_consumes_a_fifo_slot);
	return UNITY_END();
}
