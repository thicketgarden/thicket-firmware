// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// First-light bring-up for the Sharp LS027B7DH01 memory LCD.
//
// Compiled only under -DTHICKET_DISPLAY_BRINGUP; main.cpp guards the normal
// firmware off under the same flag, so this file provides setup()/loop() and
// nothing else runs. The point is the fewest moving parts: no radio, no
// filesystem, no LXMF, so a blank screen has only the display path to blame.
//
// The panel gets its own software (bit-bang) SPI on the breakout header GPIOs.
// Both hardware SPI peripherals are already taken: one is the flash bus, the
// other the LoRa radio.

#ifdef THICKET_DISPLAY_BRINGUP

#include <Arduino.h>
#undef abs
#undef round

// USB-CDC Serial is undefined without the TinyUSB stack in the link on the
// Adafruit nRF52 core.
#include <Adafruit_TinyUSB.h>

#include <SharpLcd.h>

using thicket::SharpLcd;
using thicket::SharpLcdBus;

// Display pins, from variant.h. Digital outputs, driven by the Arduino core.
static const uint8_t LCD_PIN_CLK = 17;   // WB_IO1 = P0.17
static const uint8_t LCD_PIN_DI  = 34;   // WB_IO2 = P0.34
static const uint8_t LCD_PIN_CS  = 31;   // WB_A1  = P0.31, SCS is ACTIVE HIGH

// VCOM must toggle at 0.5-10 Hz even on a static image or the cell takes a DC
// bias and degrades. 1 Hz sits in the middle of the range.
static const uint32_t VCOM_PERIOD_MS = 1000;

// The panel is write-only, so the framebuffer is the only copy of the screen.
// Static, not heap: 12,000 bytes, and this build allocates nothing else large.
static uint8_t g_fb[thicket::LCD_FB_BYTES];

// Bit-bang SPI. The driver hands bytes already in the panel's bit order, so
// write() is a plain MSB-first shift: clock idle-low, set data then a rising
// edge clocks it in. digitalWrite on this core is slow enough that the effective
// rate stays well under the panel's 2 MHz ceiling with no explicit delay, which
// is fine: the panel is slow and clean edges on hookup wire matter more.
class SoftSpiBus : public SharpLcdBus {
public:
	void select(bool on) override {
		// SCS is active HIGH on this panel: HIGH selects.
		digitalWrite(LCD_PIN_CS, on ? HIGH : LOW);
	}
	void write(const uint8_t* data, size_t len) override {
		for (size_t i = 0; i < len; ++i) {
			uint8_t b = data[i];
			for (uint8_t bit = 0; bit < 8; ++bit) {
				digitalWrite(LCD_PIN_DI, (b & 0x80u) ? HIGH : LOW);
				b = (uint8_t)(b << 1);
				digitalWrite(LCD_PIN_CLK, HIGH);   // sampled on the rising edge
				digitalWrite(LCD_PIN_CLK, LOW);
			}
		}
	}
};

static SoftSpiBus g_bus;
static SharpLcd   g_lcd(g_bus, g_fb);
static uint32_t   g_last_vcom_ms = 0;

// Hand-drawn test pattern, not compose_page(): compose_page pulls the whole
// Micron parser, which is a host-side dependency and not in this env's device
// libraries. A border plus solid blocks plus one line of large text proves the
// same things first light needs: every edge is reachable (border), addressing
// and orientation are right (corner blocks differ per corner), and fine
// addressing plus bit order are right (readable text). black = true is ink.
static void draw_pattern() {
	const uint16_t W = thicket::LCD_WIDTH;    // 400
	const uint16_t H = thicket::LCD_HEIGHT;   // 240

	g_lcd.fill_white();

	// 1px full border. Two horizontal rules and two vertical bars (a 1px-wide
	// fill_rect), so all four edges have to be addressed to close the frame.
	g_lcd.draw_hline(0, 0, W, true);
	g_lcd.draw_hline(0, (uint16_t)(H - 1), W, true);
	g_lcd.fill_rect(0, 0, 1, H, true);
	g_lcd.fill_rect((uint16_t)(W - 1), 0, 1, H, true);

	// Corner blocks, each a different size so orientation is unambiguous: if the
	// panel is mirrored or rotated the sizes land in the wrong corners.
	g_lcd.fill_rect(10, 10, 40, 40, true);                          // top-left, big
	g_lcd.fill_rect((uint16_t)(W - 10 - 24), 10, 24, 24, true);     // top-right, medium
	g_lcd.fill_rect(10, (uint16_t)(H - 10 - 16), 16, 16, true);     // bottom-left, small
	g_lcd.fill_rect((uint16_t)(W - 10 - 8), (uint16_t)(H - 10 - 8), 8, 8, true);  // bottom-right, tiny

	// A dither ramp: four grey blocks left-to-right, proving pixel-level
	// addressing across a span, not just solid fills.
	for (uint8_t i = 0; i < 4; ++i) {
		const uint16_t bx = (uint16_t)(120 + i * 60);
		const uint8_t level = (uint8_t)(40 + i * 55);   // darker to lighter
		g_lcd.fill_dither(bx, 30, 50, 40, level);
	}

	// One line of large text, centred-ish. Readable text is the strongest
	// single proof: it needs correct line addressing and correct bit order at
	// once. ASCII only, which the big face carries.
	const char* line = "SHARP LCD OK";
	const uint16_t tw = (uint16_t)(g_lcd.big_text_w() * 12);   // 12 glyphs
	const uint16_t tx = (tw < W) ? (uint16_t)((W - tw) / 2) : 4;
	g_lcd.draw_text_big(tx, 130, line, true);

	// A second, smaller line to confirm the body face too.
	g_lcd.draw_text(tx, (uint16_t)(130 + g_lcd.big_text_h() + 8),
	                "border + blocks + text = wiring good", true);

	g_lcd.flush();
}

void setup() {
	Serial.begin(115200);
	uint32_t waited = 0;
	while (!Serial && waited < 5000) { delay(50); waited += 50; }

	pinMode(LCD_PIN_CLK, OUTPUT);
	pinMode(LCD_PIN_DI, OUTPUT);
	pinMode(LCD_PIN_CS, OUTPUT);
	// Idle state: clock low, data low, CS low (active HIGH, so LOW = deselected).
	digitalWrite(LCD_PIN_CLK, LOW);
	digitalWrite(LCD_PIN_DI, LOW);
	digitalWrite(LCD_PIN_CS, LOW);

	Serial.println();
	Serial.println("=========================================================");
	Serial.println(" Thicket display first-light - Sharp LS027B7DH01");
	Serial.println(" soft-SPI: CLK=P0.17 DI=P0.34 CS=P0.31 (SCS active HIGH)");
	Serial.println("=========================================================");

	// All-clear first: the panel's own clear command writes white, so a good
	// wire shows a clean white screen before any pattern.
	g_lcd.clear();
	Serial.println("cleared to white");

	draw_pattern();
	Serial.println("pattern drawn: border, corner blocks, dither ramp, text");
	Serial.println("VCOM toggling at 1 Hz to hold the image");
}

void loop() {
	const uint32_t now = millis();
	if ((uint32_t)(now - g_last_vcom_ms) >= VCOM_PERIOD_MS) {
		g_last_vcom_ms = now;
		// Required at 0.5-10 Hz even on a static image (EXTMODE tied low, so
		// software VCOM). Leaves the image untouched, sends no pixel data.
		g_lcd.toggle_vcom();
	}
}

#endif  // THICKET_DISPLAY_BRINGUP
