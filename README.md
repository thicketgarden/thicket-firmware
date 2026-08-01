# thicket-firmware

Firmware for **Thicket** — a standalone, low-power handheld communicator for the
[Reticulum](https://github.com/markqvist/Reticulum) network: nRF52840 + SX1262
(RAK4631), 915 MHz, Sharp Memory LCD, weeks-not-days battery life. The stack
runs **on the device**: on-device identity, on-device encryption, messages
composed and delivered over LoRa/LXMF without a host.

## Status

Early bring-up, pre-hardware. `src/main.cpp` is the real application skeleton:
it brings up the SX1262, mounts external SPI flash, creates or loads a
persistent Reticulum identity, starts transport, announces, and instantiates an
LXMF router. Both build environments link. **None of it has run on a board
yet** — the numbers below are link-time numbers and every one of them is a
hypothesis until a RAK4631 is on the bench.

Not yet wired, and marked `TODO(M1)` at each site in the source: message
composition and send, a message store, display, input, and conformance testing
against Python RNS.

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
| `wiscore_rak4631` | 439,936 B | 815,104 B | 53.97% | 375,168 B |
| `wiscore_rak4631-noble` | 440,080 B | 966,656 B | 45.53% | 526,576 B |

Static RAM is 25,600 B (`.data` + `.bss`) in both envs, against a linker RAM
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
- [microLXMF](https://github.com/attermann/microLXMF) — C++ LXMF messenger layer (GPL-3.0)
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
