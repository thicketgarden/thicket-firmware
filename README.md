# thicket-firmware

Firmware for **Thicket** — a standalone, low-power handheld communicator for the
[Reticulum](https://github.com/markqvist/Reticulum) network: nRF52840 + SX1262
(RAK4631), 915 MHz, Sharp Memory LCD, weeks-not-days battery life. The stack
runs **on the device**: on-device identity, on-device encryption, messages
composed and delivered over LoRa/LXMF without a host.

## Status

Early bring-up. What exists today: the `wiscore_rak4631` PlatformIO
environment builds a toolchain-proof scaffold. The microReticulum + LXMF port
is in progress — see issues/milestones.

## Building

```
pio run -e wiscore_rak4631
```

Board definition and variant for the RAK4631 are vendored in `boards/` and
`variants/` (mirrored from
[microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware)).

## Standing on

- [microReticulum](https://github.com/attermann/microReticulum) — C++ RNS (Apache-2.0)
- [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware) — RNode-style firmware w/ RAK4631 target (GPL-3.0)
- [Reticulum](https://github.com/markqvist/Reticulum) — the protocol (public domain) and reference implementation

## License

GPL-3.0-or-later. See `LICENSE`.
