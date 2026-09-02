# LoRaInterface

`RNS::InterfaceImpl` over RadioLib, the LoRa PHY that Reticulum transport
sits on. Not written here.

## Origin

| | |
|---|---|
| Upstream | [attermann/microReticulum](https://github.com/attermann/microReticulum) |
| Path | `examples/common/lora_interface/` |
| Commit | `40fa628809d57140180c1c833559ab96fec992c1` (v0.5.0) |
| Vendored | 2026-08-01 |
| License | Apache-2.0, full text in `./LICENSE`, copied from the upstream repo root |
| Copyright | © 2026 Chad Attermann |

## Why vendored instead of a `lib_deps` entry

It ships a `library.properties`, so it's a real Arduino library, but it lives
in a subdirectory of the microReticulum repository, and PlatformIO can't
depend on a subdirectory of a git repo. Upstream's own examples reach it with
`lora_interface=symlink://../common/lora_interface`, which only works from
inside that tree. Copying it in is the honest option; the alternative is a
build script that clones microReticulum twice.

Consequence: **this copy does not update when the `microReticulum` pin in
`platformio.ini` moves.** When that pin changes, re-diff against
`examples/common/lora_interface/` at the new SHA.

## Thicket modifications

Apache-2.0 §4(b) requires modified files to say so. Changes 1–3 are in
`src/LoRaInterface.cpp`, inside the `BOARD_RAK4631` branch, each marked with a
`Thicket modification` comment. Change 4 spans both files and is marked in place.

1. **Radio moved to `SPI1`.** Upstream calls
   `SPI.setPins(MISO, SCK, MOSI); SPI.begin();`, repointing the single Arduino
   `SPI` instance at the SX1262. On the RAK4631 that instance is the WisBlock
   IO-slot bus, which is where the RAK15001 external flash lives, and Thicket
   keeps identity, keys & messages there, never on the ~28 KB internal FS.
   Repointing the bus would break persistence the moment the radio started.
   `variants/rak4630/variant.h` now declares `SPI1` on the radio pins
   (P1.11/P1.12/P1.13) and this file uses it. Upstream doesn't hit the problem
   because its nRF52 examples fall back to microStore's `InternalFSFileSystem`.

2. **TCXO reference voltage is a build flag,** `THICKET_SX1262_TCXO_VOLTAGE`,
   defaulting to **1.8 V**. Upstream hard-codes 1.6 V. Meshtastic's RAK4631
   variant and microReticulum_Firmware PR #91 both say 1.8 V, and PR #91 notes
   that microReticulum_Firmware master already uses 1.8 V for every other
   SX1262 board. A wrong value here presents as `XOSC_START_ERR` at `begin()`,
   i.e. as a dead radio, so it needs to be flippable without editing vendored
   code. **TODO(bring-up): settle on the bench.**

3. **Over-current protection raised to 140 mA,** `THICKET_SX1262_OCP_MA`.
   RadioLib leaves OCP at the SX1262 reset default of 60 mA. RAK's datasheet
   draws 92 mA at +17 dBm & 125 mA at +20 dBm in PA-boost, so the default
   folds back below the module's rated output. Semtech's high-power PA setting
   is ~140 mA. The OCP register is 2.5 mA/LSB (`0x28` = 100 mA, `0x38` =
   140 mA); RadioLib's `setCurrentLimit()` takes milliamps. This raises a
   ceiling, transmit power is untouched at upstream's +17 dBm.

4. **Last-frame RSSI/SNR are readable,** `last_rssi()`, `last_snr()`,
   `signal_valid()`. Upstream reads `getRSSI()`/`getSNR()` in `loop()` and
   prints them to `Serial`, then drops them. RadioLib overwrites both on the
   next packet, so by the time a received LXMF message reaches an application
   delivery callback there is no way to ask how well it arrived. `loop()` now
   latches the two values at the same point it already printed them; the
   printing is unchanged. Thicket's auto-reply puts them in the reply body, which
   is how link quality gets read off a peer's screen with no display of our own.

   Caveat carried in the header comment: these are **frame** figures. A payload
   split across two LoRa transmissions reports the second half, and a reader
   one main-loop iteration late may see a newer, unrelated frame.

Nothing else is changed: the split-packet framing, the 915 MHz / BW 125 kHz /
SF8 / CR4:5 parameters, `setDio2AsRfSwitch(true)`, DC-DC regulator mode
(`useRegulatorLDO = false`) and the polled-IRQ receive loop are upstream's.

## Known upstream behaviour worth remembering

- `loop()` polls `checkIrq(RADIOLIB_IRQ_RX_DONE)`; there's no ISR. Receive
  latency is therefore bounded by how often the application calls
  `Reticulum::loop()`. That matters for the always-listening idle contract.
- `send_outgoing()` calls `_radio->transmit()`, which **blocks** for the whole
  air time. At SF8/125 kHz a 255-byte frame is roughly 400 ms.
- Frames larger than 254 bytes are split across two LoRa frames with a
  1-byte header (bit 3 = split flag, bits 2:0 = sequence). This is
  interface-private framing, not RNS framing.
