# thicket-firmware

Reticulum and LXMF running entirely on an nRF52840. No daemon on a laptop, no
app on a phone: the identity, the encryption, the routing & the message store
all live on the microcontroller. It runs today on a RAK4631, which is an
nRF52840 with an SX1262 radio, and it has held a two-way conversation with the
Python reference implementation over LoRa.

This is a development repository. First commit 2026-07-31, no release, no
tagged version, and the handheld it's aimed at doesn't exist as hardware yet.

## What works today

Every row below was run on a RAK4631 unless it says otherwise, and the date is
when it last ran rather than when it was written.

| capability | state | evidence, or what's missing | date |
|---|---|---|---|
| boots, brings up the SX1262, announces | works | serial capture | 2026-08-03 |
| receives an LXMF message, decrypts, auto-replies | works | serial capture | 2026-08-03 |
| talks to Python RNS 1.4.2 over LoRa | works | live exchange with `rnsd` on a Pi | 2026-08-05 |
| stores messages encrypted at rest | works | write-then-read-back on the device | 2026-08-05 |
| identity survives a power cycle | works | came back as `157c0f2c759d0b6acbd22034c0db0413`, loaded from external flash | 2026-09-01 |
| stays reachable across that cycle | works | second board, 6 delivered, 0 failed | 2026-09-01 |
| allocator pool stays bounded under load | works | 1,194 s soak, 94 inbound messages | 2026-09-01 |
| starts a conversation with a silent peer | not built | TODO at the site in `src/main.cpp` | |
| display, input, sleep | not built | TODO at each site | |
| battery life | unmeasured | needs a profiler, not an anecdote | |
| conformance suite on real hardware | unmeasured | every scenario runs on a host | |
| talks to a phone client | untested | every peer so far was a board or a Pi | |

## Running it on a RAK4631

You need a RAK4631 and a data-capable USB cable. With an RAK15001 flash module
in the IO slot, build `wiscore_rak4631` and the identity survives a power cycle.
Without one, build `wiscore_rak4631-internalfs`, which keeps state in about
28 KB of internal flash and mints a fresh identity on every boot.

```
pio run -e wiscore_rak4631-internalfs
python3 scripts/board.py flash wiscore_rak4631-internalfs
python3 scripts/board.py capture --seconds 20
```

A healthy boot prints six numbered stages, `[1/6]` through `[6/6]`, then its own
address. If the radio
comes up you'll see the band line, and once something messages that address the
device answers by itself:

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

The reply is composed, signed & encrypted on the device, and the address it goes
back to is learned from the inbound message rather than compiled in. A stock
build sends announces & replies and nothing else. Timed sending exists but
needs both `-DTHICKET_AUTOSEND_INTERVAL_S=<n>` and `-DTHICKET_AUTOSEND_DEST=<32
hex>` at build time, so you can't turn it on by accident.

`platformio.ini` defines nine environments, and four of them are the ones to
build. `wiscore_rak4631` is the real one, with SoftDevice S140 and an 815,104 B
app region; `-noble` drops BLE for 966,656 B; `-internalfs` swaps external
flash for roughly 28 KB of internal flash; `-noflash` has no filesystem at all.
The remaining five are `native` for the host tests and four instrumentation
rigs. The last two regenerate the identity every boot, so neither says anything
about persistence.

## Failure modes that look like something else

These four have each cost an afternoon. They're the reason `scripts/board.py`
exists rather than a bare `pio run -t upload`.

**A PlatformIO upload prints SUCCESS without programming anything.** The exit
code is not the signal.

`board.py` keys on `Device programmed` instead, so a
board still running yesterday's image can't be mistaken for a firmware bug, and
if you flash by hand it's the tool's output you want rather than its status.

**The boot banner is gone before a terminal can attach.** USB-CDC discards
writes when no host is listening and the firmware prints its banner once, so
`board.py` polls for the port every 20 ms to win that race, and attaching a
second late captures nothing at all, which looks exactly like a dead board.

**Do not hold the serial port open across an upload.** The DFU tool needs it,
and a second reader makes the upload fail with a message about multiple access
that reads like a bootloader version mismatch. It isn't one.

**Do not raise the allocator pool without a board on the bench.** Upstream's
own RAK4631 environment ships 204,800 B, about 78% of the nRF52840's 262,144 B
of SRAM, and that sits in a zone where the part hard-faults before USB
enumerates. The board just looks dead.

Thicket sets 98,304 B. The reasoning behind that number is written out in
`platformio.ini`, and it's worth reading before anyone changes it.

## The numbers, and what measures them

Flash comes from the Intel HEX rather than from `pio run`, whose Flash figure
omits `.ARM.extab`. That section isn't small on a stack that throws.

| env | image | app region | used | free |
|---|---|---|---|---|
| `wiscore_rak4631` | 467,980 B | 815,104 B | 57.41% | 347,124 B |
| `wiscore_rak4631-noble` | 468,124 B | 966,656 B | 48.43% | 498,532 B |

Static RAM is 56,620 B, `.data` plus `.bss`, against a linker region of
262,136 B. Run `scripts/ramreport.py` for the breakdown and `scripts/hexsize.py`
for the table above.

Read static RAM from the ELF section table rather than from
`arm-none-eabi-size`, which reports the linker's `.heap` inside `bss` and so
makes this firmware look like 232 KB of static RAM about to overflow. It isn't.

`.heap` is a budget the
linker sizes to fill whatever is left, currently 203,468 B, so it grows when
static RAM shrinks.

The Reticulum allocator pool takes a further 98,304 B out of that heap at
runtime. No static size report shows it. It was 65,536 B during bring-up and
got raised once there was a board to measure on, because fragmentation tracks
message traffic rather than uptime, and at 64 KiB it went from 2% at boot to
16% after a single inbound message and reply.

That 16% is where it settles, not a point on a climb. A 1,194 s soak with 94
inbound messages arriving over LoRa from a second RAK4631 held fragmentation
flat, 15.99% at the start against 15.92% at the end, with a 19.64% worst sample
and a high-water mark of 60,308 B of the 98,304 B pool. Idle over the same
window sat at 1.7%.

`test_interop/run_pool_soak.sh` runs the same measurement on a host and returns
a plateau-or-climb verdict against stated thresholds, and over 2,000 cycles mean
use moves from 24,167 B to 24,164 B while fragmentation holds flat at 2.13%. It
covers the outbound path only, and prints that as a coverage warning at the end
of every run, because with no peer nothing is ever delivered.

Pool sizing isn't settled. The 61% high-water mark suggests headroom to
reclaim, but shrinking it needs a board attached and a second traffic profile
first, since one peer every 20 seconds isn't the same load as several peers
holding concurrent links.

Application work runs in a task with an 8,192 B stack rather than in `loop()`,
whose stack the Arduino core fixes at 4,096 B and doesn't expose to a build
flag.

That's not a precaution. After 51 inbound messages the task reported
3,324 B free at its low-water mark, so peak depth was 4,868 B, and the 4,096 B
ceiling would have been gone before the deepest frame ran.
`scripts/stackusage.py` cross-checks compiler frame sizes against the linked
image.

## Talking to the reference implementation

On 2026-08-05 this firmware exchanged messages with Python RNS 1.4.2 and LXMF,
running `rnsd` on a Raspberry Pi with an RNode as its radio. Inbound arrived
DIRECT over an RNS Link and was unpacked, source identity resolved, signature
validated & delivery proof returned, and the reply went back OPPORTUNISTIC to
come home as `DELIVERED (proof received)` at -73 dBm and SNR 12.75 dB.

The same exchange was repeated on 2026-08-05 with the board on its own battery,
nothing attached. The message went out and the reply came back. That run was
watched at the bench rather than captured, because untethered means no serial
line to record it, so it's reported here rather than shown.

That matters more than the earlier round trip, which ran against a client built
on the same microReticulum pin in `platformio.ini`. Two nodes of one
implementation can misread the protocol identically and agree perfectly, so
with the reference standing as the specification, agreement with it is the only
compatibility claim worth making.

`test_interop/` holds eleven scenarios. In five the Python side originates and
this stack has to receive, which is the direction a handheld lives in: a cold
inbound packet, an LXMF delivery, a Python-initiated link, a packet relayed
through a transport node, and two reference peers reaching each other through
this stack. The other six check encoding and behaviour directly, including
upstream's own `Examples/Echo.py` run unmodified.

```
PATH="/path/to/venv/bin:$PATH" bash test_interop/run_all.sh
```

All eleven run in CI on every push against pinned `rns==1.4.2` and
`lxmf==1.1.1`, and each runner takes `--self-test-break` to corrupt an input
and prove its assertions fail, because an assertion nobody has ever watched
fail isn't evidence.

`docs/parity-matrix.md` maps the stack module by module against the Python
reference. Eleven Reticulum rows: six carry evidence, one is unassessed, and
four have no counterpart on the C++ side to compare against, which is a
different statement from untested, and the table keeps the two apart. Read the
Evidence column, not the Present column.

## What the encryption protects

Messages are stored encrypted with AES-256-CTR and authenticated with
HMAC-SHA256, under keys derived from the device's own X25519 private key. No
passphrase is involved. Nothing waits on a user-interface decision.

Because those keys derive from the identity, deleting the identity makes every
stored message unreadable at once. That's crypto-erase. One key, deleted, with
no wipe & no overwrite passes. On a sealed handheld with no wipe button, that's
the difference between destroying one key and overwriting the 37,384 B the
message store occupies.

Bring-up refuses to report the store attached until it has encrypted, written,
read back and decrypted a file on the real filesystem. All four steps. That
check exists because the device once reported a healthy store while every save
into its 28 KB internal-flash filesystem was failing. That filesystem was full,
and three messages arrived, were answered correctly, and went nowhere.

The identity itself is still a plaintext file on the same flash, so anyone who
images the whole device reads it and derives the AES-256-CTR message keys from
it. What the encryption buys is a store that stays unreadable to anyone who
gets the flash without that identity file. Closing the rest needs a passphrase-
protected identity, and this device has nowhere to type one.

Check your own silicon before trusting internal flash. nRF52840 modules ship in
several build codes and the firmware prints its own at boot, as `part=` and
`variant=`. One RAK4631 on this bench reads `variant=AAD0`, a Dxx-class part
with `UICR.APPROTECT` erased to `0xFFFFFFFF`, so on that board the debug port
is open and internal flash reads out over SWD with no attack required.

On Fxx and later, lifting protection takes two independent actions,
`UICR.APPROTECT` programmed to HwDisabled & firmware writing
`APPROTECT.DISABLE`, and the 2020 LimitedResults voltage glitch defeats the
first while it can't perform the second, so the port stays shut for exactly
one reason, which is that no code here does the software write.
`scripts/check_approtect.py` asserts that at build time, because an absence
rots silently.

## The handheld this is aimed at

None of this exists. No schematic, no layout, and most part choices unvalidated
against datasheets. The RAK4631 module is the only part here that has been
bought and run.

The target is a custom carrier board around a socketed RAK4631, kept as a
module rather than a redesign because the module carries the FCC modular grant.
A 915 MHz antenna leaves through a bulkhead SMA. SPI carries a Sharp 400×240
1-bit memory LCD, write-only, so its 12,000 B framebuffer has to live in MCU
RAM, plus external flash for the identity & the message store.

I2C carries a keypad scanner, a haptic driver & LRA, a magnetic encoder reading
the thumbwheel through a sealed wall, a fuel gauge, and an LED driver for the
bargraph. Hall sensors, the piezo & the backlight sit on GPIO and PWM, and
power is a single 18650 behind a two-input charger, with a sealed IP67 USB-C
service port and a pogo dock for daily charging. NFC coil pads are reserved and
unpopulated.

The board definition in `boards/` and the variant in `variants/` describe the
development rig rather than that carrier board, and where the two disagree the
vendored variant is what the firmware builds against.

## What this is built on

- [microReticulum](https://github.com/attermann/microReticulum), C++ RNS, Apache-2.0
- [microLXMF](https://github.com/torlando-tech/microLXMF), C++ LXMF, GPL-3.0
- [microStore](https://github.com/attermann/microStore), embedded key-value persistence, Apache-2.0
- [RadioLib](https://github.com/jgromes/RadioLib), SX1262 driver, MIT
- [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware), RNode-style firmware with a RAK4631 target, GPL-3.0
- [Reticulum](https://github.com/markqvist/Reticulum), the protocol & its reference implementation

Reticulum ships under the [Reticulum
License](https://github.com/markqvist/Reticulum/blob/master/LICENSE), MIT terms
plus two restrictions: no use in systems able to purposefully harm people, and
no use contributing to AI or machine-learning training. It isn't OSI-approved
and GitHub doesn't classify it.

Thicket ships none of that code. It's the specification this firmware conforms
to, and CI installs `rns==1.4.2` to act as the peer those tests are checked
against.

`lib/LoRaInterface/` is vendored from microReticulum's
`examples/common/lora_interface/`, Apache-2.0, because PlatformIO can't depend
on a subdirectory of a repository. Origin commit & local changes are listed in
`lib/LoRaInterface/README.md`.

Dependencies are pinned to explicit commits in `platformio.ini`, and
`scripts/patch_deps.py` applies portability patches at build time and fails
loudly if a pin moves underneath it.

## License

GPL-3.0-or-later. See `LICENSE`.

## AI use

Most of this code, and most of this README, was written by an LLM working under
a human maintainer who reviews and rules on every commit.

It belongs at the top of your judgement rather than in a footnote, because an
LLM will produce something that compiles, passes its own tests, and doesn't
interoperate. That failure is silent. The controls above exist because of it,
and they're the part worth judging: pinned reference versions in CI, a wire
oracle that decodes this stack's packets with the reference's own `RNS.Packet`
and compares field by field, runners that can be told to break themselves, and
a parity page that names its own gaps.

If something in this repository looks wrong, it is. `docs/parity-matrix.md`
lists four Reticulum modules with no counterpart on this side, and that page is
the honest one. Open an issue.