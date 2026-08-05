# thicket-firmware

A standalone [Reticulum](https://github.com/markqvist/Reticulum) and LXMF node
on an nRF52840 with an SX1262, running at 915 MHz on a RAK4631. No daemon on a
laptop and no app on a phone. Identity, encryption, routing and messaging all
run on the microcontroller, built on
[microReticulum](https://github.com/attermann/microReticulum) and
[microLXMF](https://github.com/torlando-tech/microLXMF).

Thicket is the firmware. A handheld is its first target, and that hardware does
not exist yet.

## Status

**It runs on hardware.** On 2026-08-03 a RAK4631 booted this firmware, brought
up the SX1262 at 914.875 MHz, announced an LXMF delivery destination, received a
message from a peer, decrypted it, and answered. The peer returned a delivery
proof, so the reply arrived. Nothing was tethered.

The log of that run:

```
[3/6] SX1262 radio (LoRaInterface)
      band: 914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm
[INF] LoRa init succeeded.
[6/6] Announce
[INF] Announce sent successfully

--- LXMF message received ---
  from   : 0795eea0982493718c1d5dac3bf6f0d0
  rssi   : -46.00 dBm   snr : 13.50 dB
  content: hi
--- LXMF message send ---
  reason : auto-reply
  body   : #0 up73s r-46 s+13.5 id6a74c787
[INF] Message sent via OPPORTUNISTIC delivery
LXMF: DELIVERED (proof received) b2715d15d4a106f9...
```

`src/main.cpp` is the application. It brings up the SX1262, mounts storage,
creates or loads a Reticulum identity, announces, runs an LXMF router, and
answers messages sent to its `lxmf.delivery` address. The reply is composed,
signed and encrypted on the device. The address it goes to is learned from the
inbound message, never compiled in.

The reply body is diagnostic: a counter, uptime, the RSSI and SNR of the frame
that arrived, and the first bytes of this device's identity hash, sized to fit
one LoRa packet. The board has no screen and no buttons, so the peer's screen is
the output device.

A timed send exists as a fallback and is **off by default**. It requires both
`-DTHICKET_AUTOSEND_INTERVAL_S=<n>` and `-DTHICKET_AUTOSEND_DEST=<32 hex chars>`
at build time. A stock build transmits announces and replies, never unsolicited
traffic.

### What that run did not prove

**Persistence.** The round trip above ran under `wiscore_rak4631-internalfs`,
a bring-up environment that keeps state in internal flash and regenerates the
identity every boot. The identity surviving a power cycle is the other half of
the done-condition and it needs a RAK15001 in the IO slot, which has not been
tested.

**Power.** The current firmware busy-loops the CPU. No battery figure is
claimed, and none will be until a board and a profiler produce one.

**Not yet wired**, and marked with a TODO at each site in the source: a message
store, initiating a conversation with a peer that has not written first,
display, input, and sleep.

### One finding from first boot

**This part cannot be locked.** The silicon reports `variant=AAD0`, a
Dxx-class part, and `UICR.APPROTECT` reads `0xFFFFFFFF`, erased. The debug port
is open and internal flash is readable over SWD with no attack required. That is
a property of the part, not of this firmware, and no firmware change can alter
it. Anything that depends on a secret in internal flash needs Fxx+ silicon.
`scripts/check_approtect.py` refuses to build firmware that writes
`APPROTECT.DISABLE`, which remains correct and is not sufficient on its own.

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

## Conformance

`docs/parity-matrix.md` maps the stack we ship, microReticulum and microLXMF at
the pins in `platformio.ini`, against the Python reference, module by module,
with an evidence column.

Of the eleven Reticulum rows, six carry evidence, one is unassessed, and four
have no counterpart on our side to compare at all. The page exists because the
manual defines Reticulum as full interoperability and sufficient functional
parity with the reference — so a coverage map that names its gaps is worth more
than a claim that cannot be checked. Read the Evidence column, not the Present
column: a populated row is not a passing row.

`test_interop/` holds six scenarios in which the **Python side originates** and
this stack has to receive: a cold inbound packet, an LXMF delivery, identity
vectors, a Python-initiated link, a packet relayed to us through a transport
node, and two reference peers reaching each other *through* us. That direction
is the one that matters for a handheld, which spends its day being reached
rather than transmitting. They run in CI against pinned `rns==1.4.2` and
`lxmf==1.1.1`, and each has been shown to fail when the behaviour it tests is
broken.

```
PATH="/path/to/venv/bin:$PATH" bash test_interop/run_all.sh
```

These run on hosts. No conformance scenario has yet executed on a RAK4631 —
the device evidence above and the parity evidence here are still separate
claims.

## Building

```
pio run -e wiscore_rak4631          # SoftDevice S140, 815,104 B app region
pio run -e wiscore_rak4631-noble    # no BLE, 966,656 B app region
```

Both must stay green. Two further environments exist for bring-up on a board
with no RAK15001 in the IO slot. Neither persists anything and neither should
be used for anything else:

```
pio run -e wiscore_rak4631-noflash      # no filesystem at all
pio run -e wiscore_rak4631-internalfs   # ~28 KB internal flash instead
```

`-noflash` reaches the radio and announces, but `Identity::remember()` has
nowhere to write, so no path is ever stored and a reply can be composed and
never sent. `-internalfs` gives it somewhere real to write, which is what the
round trip above needed. Both regenerate the identity every boot.

### Flashing

Use `scripts/board.py`, not `pio run -t upload` directly:

```
python3 scripts/board.py flash wiscore_rak4631-internalfs
python3 scripts/board.py capture --seconds 20
```

It exists because two failure modes here look like something else. **A
PlatformIO upload can print SUCCESS without programming anything** — it keys on
`Device programmed` instead of the exit code, so a board still running the
previous image cannot be mistaken for a firmware bug. And **the boot banner is
gone before a terminal can attach**: USB-CDC discards writes with no host
listening and the firmware prints once, so it polls for the port every 20 ms to
win that race. Attaching a second later captures nothing, which reads exactly
like a dead board.

Do not hold the serial port open across an upload. The DFU tool needs it, and a
second reader makes it fail with a message about multiple access that reads like
a bootloader version mismatch and is not.

Dependencies are pinned to explicit commits in
`platformio.ini`; `scripts/patch_deps.py` applies portability patches to the
fetched sources at build time and fails loudly if a pin moves under it.

Size, measured from the Intel HEX by `scripts/hexsize.py` (`pio run`'s own
Flash figure omits `.ARM.extab`, which is not small on a stack that throws):

| env | image | app region | used | free |
|---|---|---|---|---|
| `wiscore_rak4631` | 434,688 B | 815,104 B | 53.33% | 380,416 B |
| `wiscore_rak4631-noble` | 434,832 B | 966,656 B | 44.98% | 531,824 B |

Static RAM is 19,156 B (`.data` + `.bss`) in both envs, against a linker RAM
region of 262,136 B.

Two notes on reading those numbers, both of which have caused wrong conclusions
here:

- **`arm-none-eabi-size` is misleading on this target.** It reports the
  linker's `.heap` section inside `bss`, so this firmware reads as roughly
  232 KB of static RAM and looks about to overflow. It is not. Read the ELF
  section table — `scripts/ramreport.py` does, and attributes `.data`/`.bss`
  per origin the way `scripts/mapsize.py` does for flash.
- **`.heap` is a budget, not consumption.** The linker sizes it to fill
  whatever is left, so it grows when static RAM shrinks. It is 240,932 B here.

The Reticulum allocator pool is a further 98,304 B taken from that heap at
runtime, so it appears in no static size report. It was 65,536 B during
bring-up; it was raised once there was a board to measure on, because
fragmentation tracks message traffic rather than uptime — at 64 KiB the pool
went from 2% fragmented at boot to 16% after a single inbound message and
reply, and at 96 KiB the same load produces 8%.

**Measured on the board** after full bring-up, with the radio in continuous
receive and the messaging layer live: 120,964 B of heap in use, 48,360 B of
allocator pool free, and 116,040 B of system heap still spare. Do not raise the
pool further without a board on the bench — a pool above roughly 75% of SRAM
hard-faults before USB enumerates, and the board then simply looks dead.

The application's work runs in a task the firmware creates with an 8,192 B
stack rather than in `loop()`, whose stack the Arduino core fixes at 4,096 B
and does not expose to a build flag. That is not a precaution: receiving a
message and composing a reply has been measured on the board at up to 4,704 B
of stack, which the 4,096 B ceiling could not have survived. `scripts/stackusage.py`
reports worst-case frames from the compiler and cross-checks them against the
linked image, so garbage-collected code is not mistaken for a live hazard.

Board definition and variant for the RAK4631 are vendored in `boards/` and
`variants/` (mirrored from
[microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware);
`variants/rak4630/variant.h` carries one local change, marked in place, giving
the radio its own SPI instance so it cannot steal the external-flash bus).

## Standing on

- [microReticulum](https://github.com/attermann/microReticulum), C++ RNS (Apache-2.0)
- [microLXMF](https://github.com/torlando-tech/microLXMF), C++ LXMF messenger layer (GPL-3.0)
- [microStore](https://github.com/attermann/microStore), embedded key-value persistence (Apache-2.0)
- [RadioLib](https://github.com/jgromes/RadioLib), SX1262 driver (MIT)
- [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware), RNode-style firmware w/ RAK4631 target (GPL-3.0)
- [Reticulum](https://github.com/markqvist/Reticulum), the protocol and its reference
  implementation, under the custom
  [Reticulum License](https://github.com/markqvist/Reticulum/blob/master/LICENSE):
  MIT terms plus two restrictions, no use in systems able to purposefully harm
  people, and no use contributing to AI or machine-learning training. Not an
  OSI-approved licence, and GitHub does not classify it. We ship none of this
  code: it is the specification we conform to, and CI installs it to run as the
  peer our interop tests are checked against.

`lib/LoRaInterface/` is vendored from microReticulum's
`examples/common/lora_interface/` (Apache-2.0) because PlatformIO cannot depend
on a subdirectory of a repository. Origin commit and the list of local changes
are in `lib/LoRaInterface/README.md`.

## License

GPL-3.0-or-later. See `LICENSE`.
