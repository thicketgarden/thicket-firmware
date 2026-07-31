# thicket-firmware

Firmware for Thicket, a standalone handheld Reticulum/LXMF communicator:
RAK4631 (nRF52840 + SX1262), 915 MHz US, Sharp Memory LCD 400x240 1-bit UI.
Goal: the full RNS/LXMF stack running ON the device — identity, encryption,
message composition and delivery on-device (this is not an RNode; the host is
the device).

## Build

- PlatformIO. The gate for every change: `pio run -e wiscore_rak4631` green.
- Board def + variant live in `boards/` and `variants/` (mirrored from
  attermann/microReticulum_Firmware — stock nordicnrf52 has no RAK4630/4631).
- `#include <Adafruit_TinyUSB.h>` is required wherever USB-CDC Serial is used,
  or the link fails with undefined `Adafruit_USBD_CDC` references.

## Architecture conventions

- Stack: microReticulum (Apache-2.0, C++ RNS) + an LXMF messenger layer +
  hand-rolled 1-bit UI. Multi-stack friendly: hardware should later accept
  Meshtastic firmware; Sharp LCD driver + board variant get contributed
  upstream.
- Persistence: NEVER the nRF internal filesystem (~28 KB limit). All durable
  state — identity, keys, message store — goes to external SPI flash.
- Adafruit nRF52 BSP: the SoftDevice owns flash regions and IRQ priorities.
  For SX1262 issues check init order, OCP, and regulator mode first
  (see microReticulum_Firmware branch `rak4631`).
- Determinism: scripts build/flash; changes are judged by build + on-target
  behavior, not by reading tea leaves.

## License

GPL-3.0-or-later (matches microReticulum_Firmware, our derivation base).
Keep third-party notices intact when carrying code along.
