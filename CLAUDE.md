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
  **There is a second, sharper reason than capacity:** an nRF52840 flash erase
  takes ~85 ms with interrupts disabled, which makes RadioLib's SPI commands to
  the SX1262 time out (-5). Persisting paths during the announce rebroadcast
  chain therefore breaks the *radio*, not just storage. Measured and fixed
  upstream in the only shipping nRF52840 Reticulum firmware.
- **Heap pool sizing is a hard-fault landmine.** A pool above ~75% of SRAM
  faults silently *before USB enumerates*, so it looks like a dead board.
  Working reference uses `RNS_HEAP_POOL_BUFFER_SIZE=98304` (96 KiB), or 80 KiB
  with BLE active; start at 64 KiB when bringing up. **microReticulum's own
  `wiscore_rak4631` env ships 204800 (200 KiB ≈ 78% of SRAM) — that is inside
  the fault zone. Do not adopt the upstream default unmodified.**
- **Flash, not RAM, is the binding constraint.** A transport-only build — no
  display, no font, no UI, no message store, no LXMF receive — measures
  **86.1% of the 815 KB app region**, leaving ~110 KiB for everything Thicket
  adds. Budget flash first; see `docs/nrf-prior-art.md` in hq.
- **BLE and LoRa do not coexist** in the only working reference: it suspends the
  entire Reticulum stack whenever a BLE client is connected, to avoid SoftDevice
  supervision timeouts. Assume this constrains any BLE-based feature.
- Adafruit nRF52 BSP: the SoftDevice owns flash regions and IRQ priorities.
- **"RAK4631 won't transmit" — read this before debugging RF.** The root cause
  reported upstream was **interference avoidance**, inherited from
  RNode_Firmware, which the maintainer says "seems to completely hose nRF52
  boards". Build with `-DDISABLE_IA` (on microReticulum_Firmware master since
  2026-07-09; **not** in the 1.86.4 tag — flashing that release means running
  `rnodeconf --ia-disable` by hand).
- The `rak4631` branch is **contested, unmerged, and disowned by its author**
  ("I felt like I was barking up the wrong tree"); the reporter who
  cherry-picked it later retracted. Do not cite it as the fix. The live,
  better-sourced work is **open PR #91** (RadioLib/Meshtastic backport): TCXO
  3.3 V → 1.8 V for RAK4631 — master sets 1.8 V for every *other* SX1262 board,
  so this looks like an oversight — plus Semtech erratum 15.3 RTC-stop and OCP
  140 mA. Note the OCP register is 2.5 mA/LSB, so `0x28` is 100 mA and `0x38`
  is 140 mA; the branch's own commit message miscounts this.
- **OCP is a power ceiling, not just a safety setting.** RAK's datasheet rates
  the module at **22 dBm in PA Boost mode**, drawing **125 mA at 20 dBm**
  (92 mA at 17 dBm). Semtech's setting for high-power PA operation is ~140 mA.
  microReticulum master ships OCP at `0x28` = **100 mA**, which is below what
  +22 dBm draws — so **the stock firmware caps output below the hardware's
  capability**, and over-current foldback is a plausible contributor to the
  "RAK4631 won't transmit" reports. PR #91's 140 mA is what unlocks full power.
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
