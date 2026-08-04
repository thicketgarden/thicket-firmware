// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// The keyboard's logic layer: key events in, characters and actions out.
//
// Deliberately knows nothing about the TCA8418, I2C, or the matrix. It takes
// key-down / key-up by key code and nothing else, so the whole of it runs on
// the host and can be tested before any keyboard hardware exists — which is the
// case today.
//
// ---------------------------------------------------------------------------
// WHY ONE-SHOT MODIFIERS ARE NOT A UX PREFERENCE
//
// The carrier PCB has NO PER-KEY DIODES. A diode-less matrix ghosts when three
// keys are held at once: three keys forming an L produce a phantom fourth, and
// the scanner cannot tell the phantom from a real press. That was accepted
// deliberately, on the grounds that shift, alt and sym are ONE-SHOT — pressed
// and released before the key they modify, as BlackBerry did — so a three-key
// chord never arises in normal use and the 34 diodes are not needed.
//
// **If held modifiers or chording are ever introduced here, the board is
// wrong.** There is no firmware fix; it needs diodes and a board spin. That is
// a hardware dependency carried in software, which is the kind that gets
// forgotten, so this file guards it rather than commenting on it: three or more
// simultaneous keys are refused as a combination and counted, and there are
// host tests that fail if that stops being true.
// ---------------------------------------------------------------------------

#ifndef THICKET_INPUT_LAYER_H
#define THICKET_INPUT_LAYER_H

#include <cstddef>
#include <cstdint>

namespace thicket {

// Key codes are the layer's own, not the matrix's. The scanner driver maps
// (row, column) to one of these; changing the wiring must not change this file.
enum : uint8_t {
	KEY_NONE = 0,
	// Modifiers. These are the only keys with one-shot behaviour.
	KEY_SHIFT = 1,
	KEY_ALT = 2,
	KEY_SYM = 3,
	// Everything at or above this is an ordinary key.
	KEY_FIRST_ORDINARY = 8,
};

enum Mod : uint8_t {
	MOD_NONE = 0,
	MOD_SHIFT = 1 << 0,
	MOD_ALT = 1 << 1,
	MOD_SYM = 1 << 2,
};

struct Event {
	enum Type : uint8_t {
		NONE,     // nothing to report
		CHAR,     // a character was produced
		ACTION,   // a non-character key (enter, backspace…)
		REFUSED,  // a combination was refused; see ghosting above
	};
	Type type = NONE;
	char ch = 0;         // valid when type == CHAR
	uint8_t code = 0;    // the key that caused it
	uint8_t mods = 0;    // modifiers applied, for CHAR/ACTION
};

// Maps a key code plus applied modifiers to a character. Supplied by the
// caller so the layout is data, not code, and so tests can use a small map.
using KeymapFn = char (*)(uint8_t code, uint8_t mods, void* ctx);

class InputLayer {
public:
	// Beyond this many keys physically down, the matrix reading cannot be
	// trusted on a diode-less board. Two is the most that normal one-shot
	// typing produces, and it only happens on rollover between consecutive
	// keys.
	static constexpr uint8_t GHOST_THRESHOLD = 3;
	static constexpr uint8_t MAX_TRACKED = 8;

	InputLayer(KeymapFn keymap, void* ctx) : _keymap(keymap), _ctx(ctx) {}

	Event key_down(uint8_t code);
	void key_up(uint8_t code);

	// Modifiers armed and waiting to be consumed by the next ordinary key.
	uint8_t pending_mods() const { return _pending; }
	uint8_t keys_down() const { return _down_count; }

	// True while enough keys are held that the matrix could be lying.
	bool ghost_risk() const { return _down_count >= GHOST_THRESHOLD; }

	// How many key-downs have been refused because of the above. A number
	// that climbs in the field means either the guard is too tight or someone
	// is chording, and the second one means the board is wrong.
	uint32_t refused() const { return _refused; }

	void reset();

private:
	bool is_modifier(uint8_t code) const {
		return code == KEY_SHIFT || code == KEY_ALT || code == KEY_SYM;
	}
	static uint8_t mod_bit(uint8_t code);
	bool track_down(uint8_t code);
	bool track_up(uint8_t code);

	KeymapFn _keymap;
	void* _ctx;
	uint8_t _pending = MOD_NONE;
	uint8_t _down[MAX_TRACKED] = {0};
	uint8_t _down_count = 0;
	uint32_t _refused = 0;
};

}  // namespace thicket

#endif  // THICKET_INPUT_LAYER_H
