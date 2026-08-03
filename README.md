# thicket-firmware

Firmware for the **Thicket Handheld** — a standalone, low-power communicator for the
[Reticulum](https://github.com/markqvist/Reticulum) network: nRF52840 + SX1262
(RAK4631), 915 MHz, Sharp Memory LCD, always listening. The stack
runs **on the device**: on-device identity, on-device encryption, messages
composed and delivered over LoRa/LXMF without a host.

## Status

Early bring-up, pre-hardware. `src/main.cpp` is the real application: it brings
up the SX1262, mounts external SPI flash, creates or loads a persistent
Reticulum identity, starts transport, announces, runs an LXMF router — and
**answers messages sent to it**. A message delivered to its `lxmf.delivery`
address gets a reply composed, signed and encrypted on the device and sent back
over LoRa. The reply address is learned from the inbound message, so nothing
about the peer is compiled in.

That is deliberately the shape of the proof. The board has no screen and no
buttons yet; the peer's screen is the output device, and a reply arriving there
demonstrates on-device identity, on-device encryption and LoRa delivery with
nothing tethered. The reply body is diagnostic — a counter, uptime, the RSSI
and SNR of the frame that arrived, and the first bytes of this device's
identity hash — sized to fit a single LoRa packet.

A timed send exists as a fallback and is **off by default**; it requires both
`-DTHICKET_AUTOSEND_INTERVAL_S=<n>` and `-DTHICKET_AUTOSEND_DEST=<32 hex chars>` at
build time. A stock build transmits announces and replies, never unsolicited
traffic.

**Always listening** is the design contract, not an aspiration deferred to
later: the receiver stays on, so a message arrives when it is sent rather than
when the device next polls or syncs. That is also the reason for the part
choice. ESP32-class standalone Reticulum handhelds are two-to-three-day
devices; low-power silicon plus a memory LCD that holds an image without redraw
is what makes staying awake affordable at all. The design target is on the
order of **two weeks of continuous listening on an 18650**, derived from
datasheet figures — SX1262 receive in DC-DC mode, nRF52840 in System-ON idle,
static memory LCD — and **not measured**. The real budget is still being
derived and no figure is claimed until hardware produces one. The current
firmware busy-loops the CPU and would not meet any of it.

**None of this has run on a board yet.** Both build environments link; the
numbers below are link-time numbers and every one of them is a hypothesis until
a RAK4631 is on the bench.

Not yet wired, and marked with a TODO at each site in the source: a message
store, initiating a conversation with a peer that has not written first,
display, input, sleep, and conformance testing against Python RNS.

## Hardware

The target is a custom carrier board around a **socketed RAK4631**, which stays
a module rather than a redesign because it carries the FCC modular grant. A
915 MHz antenna leaves through a bulkhead SMA. On SPI: a Sharp 400×240 1-bit
memory LCD, which is write-only, so its 13.5 KB framebuffer has to live in MCU
RAM, and external flash for identity and the message store. On I2C: a keypad
scanner, a haptic driver and LRA, a magnetic encoder reading the thumbwheel
through a sealed wall, a fuel gauge, and an LED driver for the bargraph. Hall
sensors, the piezo and the backlight sit on GPIO and PWM. Power is a single
18650 behind a charger with two inputs, a sealed IP67 USB-C service port and a
pogo dock for daily charging. NFC coil pads are reserved and unpopulated.

**Nothing here has been fabricated.** No schematic exists, no board has been
laid out, and most part choices are unvalidated against datasheets. The full
block diagram and its caveats live in the mechanical and electrical repo, at
`thicket-hardware/docs/architecture.md`.

> That repository is **not published yet**, so the path above is not a link.
> This section will point at it once it is.

The board definition and variant vendored in `boards/` and `variants/` describe
the **development rig**, not that carrier board. Where the two disagree, the
vendored variant is what the current firmware actually builds against.

## Building

```
pio run -e wiscore_rak4631          # SoftDevice S140, 815,104 B app region
pio run -e wiscore_rak4631-noble    # no BLE, 966,656 B app region
```

Both must stay green. Dependencies are pinned to explicit commits in
`platformio.ini`; `scripts/patch_deps.py` applies portability patches to the
fetched sources at build time and fails loudly if a pin moves under it.

Size, measured from the Intel HEX by `scripts/hexsize.py` (`pio run`'s own
Flash figure omits `.ARM.extab`, which is not small on a stack that throws):

| env | image | app region | used | free |
|---|---|---|---|---|
| `wiscore_rak4631` | 452,180 B | 815,104 B | 55.48% | 362,924 B |
| `wiscore_rak4631-noble` | 452,324 B | 966,656 B | 46.79% | 514,332 B |

Static RAM is 25,648 B (`.data` + `.bss`) in both envs, against a linker RAM
region of 237,568 B. The Reticulum heap pool is a further
65,536 B allocated at runtime, so it appears in no static size report;
`scripts/mapsize.py` prints that reminder along with per-origin attribution.

Board definition and variant for the RAK4631 are vendored in `boards/` and
`variants/` (mirrored from
[microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware);
`variants/rak4630/variant.h` carries one local change, marked in place, giving
the radio its own SPI instance so it cannot steal the external-flash bus).

## Standing on

- [microReticulum](https://github.com/attermann/microReticulum) — C++ RNS (Apache-2.0)
- [microLXMF](https://github.com/torlando-tech/microLXMF) — C++ LXMF messenger layer (GPL-3.0)
- [microStore](https://github.com/attermann/microStore) — embedded key-value persistence (Apache-2.0)
- [RadioLib](https://github.com/jgromes/RadioLib) — SX1262 driver (MIT)
- [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware) — RNode-style firmware w/ RAK4631 target (GPL-3.0)
- [Reticulum](https://github.com/markqvist/Reticulum) — the protocol (public domain) and reference implementation

`lib/LoRaInterface/` is vendored from microReticulum's
`examples/common/lora_interface/` (Apache-2.0) because PlatformIO cannot depend
on a subdirectory of a repository. Origin commit and the list of local changes
are in `lib/LoRaInterface/README.md`.

## License

GPL-3.0-or-later. See `LICENSE`.
