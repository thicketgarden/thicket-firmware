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

**GPL-3.0-or-later** (matches microReticulum_Firmware, our derivation base).
Keep third-party notices intact when carrying code along. Source files carry
the standard GPL notice.

Shipping firmware on hardware means conveying a binary: publish the full
Corresponding Source for every shipped version, keep GPL notices visible in
docs and UI, and never lock the bootloader — buyers must be able to flash
modified firmware.

### Ratspeak: study only

All Ratspeak project code (`ratspeak/Ratspeak`, `ratspeak/rsCardputer`) is
**AGPL-3.0-or-later**. AGPL §13's network-source obligation propagates to the
entire combined work, so porting any of it would convert this repo from
GPL-3.0-or-later to AGPL.

**Do not port, copy, or adapt Ratspeak code into this repo.** Reading their
code to understand the problem is fine; transcribing it is not. If a
relicensing grant ever changes this, it arrives as an explicit ruling — not as
an inference from a maintainer's friendly comment.

The messenger layer is written against **microReticulum's Apache-2.0 API** and
the LXMF spec. That is the plan of record, not a fallback; nothing in this repo
waits on an upstream licensing answer.

Note that `ratspeak/microReticulum` (their fork) is Apache-2.0 and is not
covered by this restriction.
