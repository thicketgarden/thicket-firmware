// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Host tests for the keyboard logic layer: `pio test -e native`.
//
// The first group tests one-shot behaviour. The second group is the one that
// matters to the hardware: the carrier PCB has no per-key diodes, so a
// three-key chord ghosts, and A49 accepted that on the explicit condition that
// modifiers are one-shot and chords therefore never occur.
//
// **These tests exist so that constraint outlives the person who wrote it.**
// If someone later makes modifiers held, or lets a three-key combination
// through, the tests below fail — which is the only thing standing between a
// plausible-looking firmware change and 34 missing diodes.

#include <unity.h>
#include "InputLayer.h"

using namespace thicket;

// A deliberately tiny layout. Real layouts are data; this only has to exercise
// the modifier arithmetic.
enum : uint8_t { K_A = KEY_FIRST_ORDINARY, K_B, K_ENTER };

static char test_keymap(uint8_t code, uint8_t mods, void*) {
	switch (code) {
		case K_A:
			if (mods & MOD_SYM) return '1';
			if (mods & MOD_ALT) return '@';
			return (mods & MOD_SHIFT) ? 'A' : 'a';
		case K_B:
			return (mods & MOD_SHIFT) ? 'B' : 'b';
		case K_ENTER:
			return 0;  // an ACTION, not a character
		default:
			return 0;
	}
}

static InputLayer make() { return InputLayer(test_keymap, nullptr); }

// Press and release, the way one-shot typing actually happens.
static Event tap(InputLayer& in, uint8_t code) {
	Event e = in.key_down(code);
	in.key_up(code);
	return e;
}

// --- one-shot behaviour ----------------------------------------------------

void test_plain_key_emits_lowercase(void) {
	InputLayer in = make();
	Event e = tap(in, K_A);
	TEST_ASSERT_EQUAL(Event::CHAR, e.type);
	TEST_ASSERT_EQUAL('a', e.ch);
	TEST_ASSERT_EQUAL(MOD_NONE, e.mods);
}

void test_modifier_emits_nothing_and_arms(void) {
	InputLayer in = make();
	Event e = tap(in, KEY_SHIFT);
	TEST_ASSERT_EQUAL(Event::NONE, e.type);
	TEST_ASSERT_EQUAL(MOD_SHIFT, in.pending_mods());
}

void test_modifier_applies_after_it_is_released(void) {
	// The defining property. The modifier is up by the time the letter goes
	// down, so the two are never held together -- which is what keeps the
	// diode-less matrix honest.
	InputLayer in = make();
	tap(in, KEY_SHIFT);
	TEST_ASSERT_EQUAL(0, in.keys_down());
	Event e = tap(in, K_A);
	TEST_ASSERT_EQUAL(Event::CHAR, e.type);
	TEST_ASSERT_EQUAL('A', e.ch);
}

void test_modifier_is_spent_by_one_key_only(void) {
	InputLayer in = make();
	tap(in, KEY_SHIFT);
	TEST_ASSERT_EQUAL('A', tap(in, K_A).ch);
	TEST_ASSERT_EQUAL(MOD_NONE, in.pending_mods());
	TEST_ASSERT_EQUAL('b', tap(in, K_B).ch);  // not 'B'
}

void test_pressing_a_modifier_twice_cancels_it(void) {
	// A mis-press is undone by repeating it, rather than by typing a character
	// to flush it away.
	InputLayer in = make();
	tap(in, KEY_SHIFT);
	tap(in, KEY_SHIFT);
	TEST_ASSERT_EQUAL(MOD_NONE, in.pending_mods());
	TEST_ASSERT_EQUAL('a', tap(in, K_A).ch);
}

void test_two_different_modifiers_both_apply(void) {
	InputLayer in = make();
	tap(in, KEY_SHIFT);
	tap(in, KEY_SYM);
	TEST_ASSERT_EQUAL(MOD_SHIFT | MOD_SYM, in.pending_mods());
	Event e = tap(in, K_A);
	TEST_ASSERT_EQUAL('1', e.ch);  // keymap gives SYM priority
	TEST_ASSERT_EQUAL(MOD_SHIFT | MOD_SYM, e.mods);
}

void test_key_with_no_character_is_an_action(void) {
	InputLayer in = make();
	Event e = tap(in, K_ENTER);
	TEST_ASSERT_EQUAL(Event::ACTION, e.type);
}

void test_repeat_down_without_up_is_not_a_new_press(void) {
	// The scanner rebroadcasts held keys. Treating each as a fresh press would
	// inflate the down-count and refuse ordinary typing.
	InputLayer in = make();
	in.key_down(K_A);
	Event again = in.key_down(K_A);
	TEST_ASSERT_EQUAL(Event::NONE, again.type);
	TEST_ASSERT_EQUAL(1, in.keys_down());
	in.key_up(K_A);
}

// --- the guard: three keys must never be a valid combination ---------------

void test_three_simultaneous_keys_are_refused(void) {
	// THE test. If this fails, the board needs 34 diodes and a respin.
	InputLayer in = make();
	in.key_down(KEY_SHIFT);
	in.key_down(KEY_ALT);
	TEST_ASSERT_FALSE(in.ghost_risk());
	Event third = in.key_down(K_A);
	TEST_ASSERT_EQUAL(Event::REFUSED, third.type);
	TEST_ASSERT_TRUE(in.ghost_risk());
	TEST_ASSERT_EQUAL(1, in.refused());
}

void test_a_refused_chord_emits_no_character(void) {
	// Refusing must mean silence, not a best guess. With three keys down we
	// cannot tell which reading is the phantom, so emitting anything at all
	// would be inventing input.
	InputLayer in = make();
	in.key_down(K_A);
	in.key_down(K_B);
	Event e = in.key_down(K_ENTER);
	TEST_ASSERT_EQUAL(Event::REFUSED, e.type);
	TEST_ASSERT_EQUAL(0, e.ch);
}

void test_two_key_rollover_still_works(void) {
	// Fast typing overlaps two keys. That is below the ghost threshold and
	// must keep working, or the guard has been set too tight to type on.
	InputLayer in = make();
	in.key_down(K_A);          // still held...
	Event b = in.key_down(K_B);  // ...when the next goes down
	TEST_ASSERT_EQUAL(Event::CHAR, b.type);
	TEST_ASSERT_EQUAL('b', b.ch);
	TEST_ASSERT_EQUAL(0, in.refused());
}

void test_recovery_after_releasing_below_the_threshold(void) {
	// A refused chord must not wedge the keyboard.
	InputLayer in = make();
	in.key_down(K_A);
	in.key_down(K_B);
	in.key_down(K_ENTER);
	TEST_ASSERT_EQUAL(1, in.refused());
	in.key_up(K_A);
	in.key_up(K_B);
	in.key_up(K_ENTER);
	TEST_ASSERT_FALSE(in.ghost_risk());
	TEST_ASSERT_EQUAL('a', tap(in, K_A).ch);
}

void test_refused_count_survives_reset(void) {
	// reset() is a UI-level operation. Zeroing the counter there would hide
	// the one signal that says someone is chording on a board that cannot.
	InputLayer in = make();
	in.key_down(K_A);
	in.key_down(K_B);
	in.key_down(K_ENTER);
	in.reset();
	TEST_ASSERT_EQUAL(1, in.refused());
	TEST_ASSERT_EQUAL(0, in.keys_down());
	TEST_ASSERT_EQUAL(MOD_NONE, in.pending_mods());
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_plain_key_emits_lowercase);
	RUN_TEST(test_modifier_emits_nothing_and_arms);
	RUN_TEST(test_modifier_applies_after_it_is_released);
	RUN_TEST(test_modifier_is_spent_by_one_key_only);
	RUN_TEST(test_pressing_a_modifier_twice_cancels_it);
	RUN_TEST(test_two_different_modifiers_both_apply);
	RUN_TEST(test_key_with_no_character_is_an_action);
	RUN_TEST(test_repeat_down_without_up_is_not_a_new_press);
	RUN_TEST(test_three_simultaneous_keys_are_refused);
	RUN_TEST(test_a_refused_chord_emits_no_character);
	RUN_TEST(test_two_key_rollover_still_works);
	RUN_TEST(test_recovery_after_releasing_below_the_threshold);
	RUN_TEST(test_refused_count_survives_reset);
	return UNITY_END();
}
