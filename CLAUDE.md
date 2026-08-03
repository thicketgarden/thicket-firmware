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
  hand-rolled 1-bit UI. Multi-stack friendly: the hardware is intended to accept
  Meshtastic firmware later, and we intend to offer the Sharp LCD driver and
  board variant upstream. Both are intentions, not commitments made to anyone.
- Persistence: NEVER the nRF internal filesystem (~28 KB limit). All durable
  state — identity, keys, message store — goes to external SPI flash.
  **Capacity settles this on its own.** A second argument circulates upstream
  that an internal-flash erase masks interrupts long enough to time out
  RadioLib's SPI to the SX1262. It comes from another project's commit message,
  we have not reproduced it, and it must not be repeated as our finding.
- **Heap pool sizing is a hard-fault landmine.** A pool above ~75% of SRAM
  faults silently *before USB enumerates*, so it looks like a dead board.
  Working reference uses `RNS_HEAP_POOL_BUFFER_SIZE=98304` (96 KiB), or 80 KiB
  with BLE active; start at 64 KiB when bringing up. **microReticulum's own
  `wiscore_rak4631` env ships 204800 (200 KiB ≈ 78% of SRAM) — that is inside
  the fault zone. Do not adopt the upstream default unmodified.**
- **Flash, not RAM, is the binding constraint.** A transport-only build — no
  display, no font, no UI, no message store, no LXMF receive — measures
  **86.1% of the 815 KB app region**, leaving ~110 KiB for everything Thicket
  adds. Budget flash first.
- **BLE and LoRa do not coexist** in the only working reference: it suspends the
  entire Reticulum stack whenever a BLE client is connected, to avoid SoftDevice
  supervision timeouts. Assume this constrains any BLE-based feature.
- Adafruit nRF52 BSP: the SoftDevice owns flash regions and IRQ priorities.
- **"RAK4631 won't transmit" — read this before debugging RF.** The cause
  identified upstream is **interference avoidance**, inherited from
  RNode_Firmware. The maintainer's own assessment, in
  [issue #55](https://github.com/attermann/microReticulum_Firmware/issues/55):
  it "seems to completely hose nRF52 boards", and "simply disabling that is the
  right solution for TX". Build with `-DDISABLE_IA`.
  **It is on master but not in a release.** `platformio.ini:205` on master sets
  it; the `1.86.4` tag does not **[verified — grepped both trees 2026-08-03]**,
  so flashing a released build still needs `rnodeconf --ia-disable` by hand.
- **The `rak4631` branch is unmerged, and it is the maintainer's own branch.**
  He did not merge it because he "felt like I was barking up the wrong tree",
  and said in the same breath that "if there are gains to be added I'll
  definitely reconsider" ([#55](https://github.com/attermann/microReticulum_Firmware/issues/55)).
  Treat it as open, not abandoned, and do not cite it as a settled fix.
  The active work is
  [PR #91](https://github.com/attermann/microReticulum_Firmware/pull/91), a
  RadioLib/Meshtastic backport: TCXO 3.3 V → 1.8 V for the RAK4631, Semtech
  erratum 15.3 RTC-stop, and OCP raised to 140 mA. Master already sets 1.8 V for
  other SX1262 boards, so the RAK4631 case looks like it was simply missed.
- **OCP is a power ceiling, not only a safety setting**, and the arithmetic is
  easy to get wrong: the register is **2.5 mA/LSB**, so `0x28` is 100 mA and
  `0x38` is 140 mA. RAK's datasheet rates the module at **22 dBm in PA Boost**,
  drawing **125 mA at 20 dBm** and 92 mA at 17 dBm. Semtech's value for
  high-power PA operation is ~140 mA. Master ships `0x28`, so the configured
  ceiling sits below what +22 dBm draws, and over-current foldback is a
  plausible contributor to the TX reports above. **This is our reading of the
  numbers, not a conclusion anyone upstream has stated** — verify it on a bench
  before relying on it.
  Do not conclude the module is limited to 15.8 dBm: that figure is the *FCC
  grant's* tested level, not a hardware limit.
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
