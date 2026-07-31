// Thicket firmware — bring-up scaffold.
// Proves the wiscore_rak4631 toolchain builds before hardware arrives.
// M1 replaces this with the microReticulum + LXMF port.

#include <Arduino.h>
// Pulls the TinyUSB stack into the link — USB-CDC Serial is undefined
// without it on the Adafruit nRF52 core under PlatformIO.
#include <Adafruit_TinyUSB.h>

static const uint32_t HEARTBEAT_MS = 1000;

void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN, OUTPUT);
}

void loop() {
  static bool on = false;
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last >= HEARTBEAT_MS) {
    last = now;
    on = !on;
    digitalWrite(LED_GREEN, on ? HIGH : LOW);
    Serial.println("THICKET scaffold: toolchain alive");
  }
}
