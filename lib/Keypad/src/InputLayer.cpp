// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// See InputLayer.h for why one-shot is a hardware dependency and not a taste.

#include "InputLayer.h"

namespace thicket {

/*static*/ uint8_t InputLayer::mod_bit(uint8_t code) {
	switch (code) {
		case KEY_SHIFT: return MOD_SHIFT;
		case KEY_ALT:   return MOD_ALT;
		case KEY_SYM:   return MOD_SYM;
		default:        return MOD_NONE;
	}
}

bool InputLayer::track_down(uint8_t code) {
	for (uint8_t i = 0; i < _down_count; i++) {
		// A repeat down for a key already held is the scanner rebroadcasting,
		// not a new press. Counting it would inflate the ghost count and
		// refuse perfectly ordinary typing.
		if (_down[i] == code) return false;
	}
	if (_down_count < MAX_TRACKED) {
		_down[_down_count++] = code;
	}
	else {
		// More keys than we track is already far past the ghost threshold;
		// count it so the state is not silently wrong.
		_down_count = MAX_TRACKED;
	}
	return true;
}

bool InputLayer::track_up(uint8_t code) {
	for (uint8_t i = 0; i < _down_count; i++) {
		if (_down[i] == code) {
			for (uint8_t j = i; j + 1 < _down_count; j++) _down[j] = _down[j + 1];
			_down_count--;
			return true;
		}
	}
	return false;
}

Event InputLayer::key_down(uint8_t code) {
	Event ev;
	ev.code = code;
	if (code == KEY_NONE) return ev;

	const bool fresh = track_down(code);
	if (!fresh) return ev;  // held-key rebroadcast; nothing new happened

	// ---------------------------------------------------------------------
	// The guard. Three keys down at once is the condition under which a
	// diode-less matrix produces phantom presses, so nothing is emitted while
	// it holds -- not even for a key that would otherwise be valid, because we
	// cannot tell whether this key IS the phantom.
	//
	// This is the line that keeps the board honest. If it is ever relaxed to
	// let a chord through, the hardware needs 34 diodes and a respin.
	// ---------------------------------------------------------------------
	if (_down_count >= GHOST_THRESHOLD) {
		_refused++;
		ev.type = Event::REFUSED;
		return ev;
	}

	const uint8_t bit = mod_bit(code);
	if (bit != MOD_NONE) {
		// One-shot: arm it and emit nothing. Pressing the same modifier again
		// disarms it, so a mistaken press is undone by repeating it rather
		// than by pressing something to flush it.
		_pending = (_pending & bit) ? (uint8_t)(_pending & ~bit)
		                            : (uint8_t)(_pending | bit);
		return ev;
	}

	// An ordinary key consumes whatever is armed, whether or not the modifier
	// is still physically held. That is what makes it one-shot rather than a
	// held modifier, and it is the property the board depends on.
	const uint8_t applied = _pending;
	_pending = MOD_NONE;
	ev.mods = applied;

	const char ch = _keymap ? _keymap(code, applied, _ctx) : 0;
	if (ch != 0) {
		ev.type = Event::CHAR;
		ev.ch = ch;
	}
	else {
		ev.type = Event::ACTION;
	}
	return ev;
}

void InputLayer::key_up(uint8_t code) {
	// Releasing a modifier deliberately does NOT clear it: one-shot means the
	// arm survives the release and is spent by the next ordinary key, so a
	// modifier is never held down alongside another key.
	track_up(code);
}

void InputLayer::reset() {
	_pending = MOD_NONE;
	_down_count = 0;
	// _refused is not cleared: it is a diagnostic counter about the board, and
	// zeroing it on a UI transition would hide exactly what it exists to show.
}

}  // namespace thicket
