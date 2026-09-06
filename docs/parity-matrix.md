# Parity matrix: what we ship, against the Python reference

The Reticulum manual is explicit that the protocol is defined "entirely and
authoritatively" by the Python reference implementation and the manual, and that
an implementation without full interoperability & sufficient functional parity
**is not Reticulum**. This page is our attempt to say, honestly & checkably,
where the stack we ship stands against that bar.

> [!IMPORTANT]
> **This is a coverage map, not a parity claim.** A row with evidence means a
> scenario exercised that surface against the reference, not that the module is
> at parity. **Read the Evidence and Residual gap columns, not the Present
> column.**
>
> Of the eleven Reticulum rows, six carry evidence, one says `none`, and four
> are `absent`, meaning no counterpart exists to compare against. `absent` and
> untested are different claims and the table keeps them apart.

**Scope.** Thicket is firmware, not a Reticulum implementation. What's assessed
here is the stack we pin & ship: **microReticulum** (C++ RNS) and
**microLXMF** (C++ LXMF). Pins are in `platformio.ini`.

**No row in the matrix below has been run on hardware.** Every result in the
table comes from CI on x86 Linux hosts.

The stack itself has run on a RAK4631, and a message composed on the device has
reached a peer over LoRa. That populates no row below, because these rows are
parity against the *reference* implementation, which is a different claim. See
"Hardware evidence".

## What this result is about: the pins it passed against

**A pin bump invalidates the interop claim until the suite is re-run and the
version it passed against is recorded.** Until 2026-08-04 nothing
recorded it, so every green run was a claim about unnamed code.

**Last full pass: 2026-09-05, all 12 scenarios, at commit `d1201cb0`:**

| | |
|---|---|
| microReticulum | `9fb4828acdd24ff1e10ec528c2d24e9cae0e8acb` |
| microStore | `c5697b85a156e1b18372d7a190136bfd2c379545` |
| microLXMF | `76ca1d930206a5992252ebcf46af4d856510d30c` |
| Python `rns` | 1.4.2 |
| Python `lxmf` | 1.1.1 |

`run_all.sh` now prints this block itself, resolved from `platformio.ini` and the
installed reference, so the record can't be forgotten separately from the run.
**If the table above disagrees with a fresh run, the table is the stale one.**

## Evidence vocabulary

Deliberately narrow, so that a claim can't be made by accident.

| Token | Means |
|---|---|
| `interop` | Exercised by one of the four microReticulum interop scenarios against **Python RNS 1.4.2**, in CI, on every push. |
| `thicket-interop` | Exercised by a scenario in **`test_interop/`** in this repo, against the official reference at pinned versions, **`rns==1.4.2` and `markqvist/lxmf==1.1.1`**, built on the microReticulum SHA we actually pin. Runs in CI on every push as the `thicket-interop` job. The Evidence column names the scenario. Every one of these has been shown to fail when the behaviour it tests is broken; see `test_interop/README.md`. |
| `lxmf-conformance` | Exercised by microLXMF's cross-implementation suite against the Python LXMF reference: **84 passed, 2 skipped**, run at both stock and our pool sizes. ⚠ **That suite builds microLXMF against `torlando-tech/microReticulum @ 6054f6ba`, not the fork we pin** (`conformance-bridge/CMakeLists.txt:57-64`, verified 2026-08-03). It's evidence about the LXMF layer; it isn't evidence about the LXMF layer on our RNS layer. `thicket-interop` is. |
| `unit` | Covered by a native unit test in this repo. |
| `none` | **Code exists; we have not verified it against the reference.** Not a claim of absence, a claim of ignorance. |
| `absent` | No counterpart in the C++ stack at the pins we ship. |

⚠ **The largest caveat is direction.** Read each scenario's *receiver* to
establish it. The driver names imply narrower coverage than what actually runs,
so they are not the thing to read. What's covered:

| scenario | direction |
|---|---|
| packet | **round trip.** C++ sends, Python replies, C++ receives & asserts payload match |
| request | C++ receives a **response** from a Python-side `/echo` handler |
| link | C++ → Python only |
| resource | C++ → Python only |

So inbound decoding isn't wholly untested. **The real gap is that no scenario
has Python originate cold.** In every case the C++ side speaks first & Python
answers. Nothing tests a Python peer initiating to a device that hasn't just
transmitted, which is precisely what a handheld does all day.

**That gap is closed for packets, LXMF delivery and links.** `test_interop/`
carries scenarios in which the Python side originates:

| scenario | direction |
|---|---|
| `run_cold_inbound.sh` | **Python originates cold.** The C++ side has announced and has never transmitted to the peer; the peer never announces at all, so the C++ side can't address it. |
| `run_lxmf_inbound.sh` | Python LXMF originates to our `lxmf.delivery`; C++ asserts the decoded fields. Not cold, LXMF signature validation needs the source identity, so the peer announces first. |
| `run_link_inbound.sh` | **Python establishes the Link**, the C++ side only responds. |
| `run_identity_vectors.sh` | No direction; fixed reference vectors. |

## Reticulum: Python `RNS` 1.4.2 → microReticulum

| Python module | Our surface | Present | Evidence | Residual gap |
|---|---|---|---|---|
| `Packet.py` | `Packet.cpp` | yes | `interop` + `thicket-interop` (`run_cold_inbound.sh`) | Cold inbound now verified: a Python peer originates 383 bytes (`ENCRYPTED_MDU`) to a destination that has only announced, and the C++ side decrypts, validates, and returns a proof the reference accepts. |
| `Link.py` | `Link.cpp` | yes | `interop` + `thicket-interop` (`run_link_inbound.sh`) | Python-initiated establish, data round trip, and keepalive **response** now verified; the reference's watchdog closes the link with `TIMEOUT` when the wire is cut. **But microReticulum has no Link watchdog of its own**, see divergence 6 below, so it never originates keepalives & never times a link out. Link *proof* validation is also disabled; see divergence 7. |
| `Resource.py` | `Resource.{h,cpp}` | yes | `interop` | Transfer scenario passes host-side. Note microLXMF's own docs report Resource transfer to `lxmd` not concluding. |
| `Destination.py` | `Destination.cpp` | yes | `interop` (request/response) | GROUP destinations unverified. |
| `Identity.py` | `Identity.cpp` | yes | `thicket-interop` (`run_identity_vectors.sh`) | Key derivation from an imported private key, identity & destination hashing, `full_hash`/`truncated_hash`, HKDF, deterministic Ed25519 signing, signature validation (including two negative cases) & decryption of a reference-produced ciphertext all match Python RNS 1.4.2. One divergence found: `Cryptography::hkdf()` ignores its `context` argument, see divergence 8. |
| `Transport.py` | `Transport.cpp` | yes | `thicket-interop` (`run_multihop_inbound.sh`, `run_transport_forward.sh`) | Covered in both roles as of 2026-08-04. **As a leaf:** a transport-enabled reference node sits between the originator & us; the path is learned from a relayed announce, the packet arrives with a non-zero hop count, and the proof returns across the relay. **As a router:** two reference peers on segments with no member in common except us reach each other through us, arriving at `hops=2`. They can't hear each other directly, so delivery is proof of forwarding. This is the only scenario running `transport_enabled(true)`, the mode our four local patches affect. In both of these the peer is the reference. **`run_two_node.sh` covers the complement**, putting our stack on both ends: one node learns the other from its announce, encrypts & sends; the other decrypts byte-for-byte & returns a proof that validates. It is **not** evidence of conformance & isn't counted as such, because two implementations that misread the protocol identically agree with each other perfectly. What it covers is narrower: the constructs we only ever *produce* and never otherwise parse. |
| `Reticulum.py` | `Reticulum.cpp` | yes | `none` | Config surface differs by construction; not assessed. |
| `Channel.py` | `Channel.{h,cpp}` | **files only** | `absent` in practice | The files exist but there's no implementation: `Channel.cpp` is 26 lines of `#include`, `Link::get_channel()` is commented out (Link.cpp:1136) and the `CHANNEL` packet-context branch of `Link::receive` is commented out (Link.cpp:1489). There's no API to open a channel, so a scenario can't be written. |
| `Buffer.py` | n/a | **absent** | `absent` | No counterpart at our pin. |
| `Discovery.py` | n/a | **absent** | `absent` | No counterpart at our pin. |
| `Resolver.py` | n/a | **absent** | `absent` | No counterpart at our pin. |

**[verified 2026-08-03, module lists read from `markqvist/Reticulum` at tag
`1.4.2` and from our pinned microReticulum tree, headers included.]**

## LXMF: Python `LXMF` → microLXMF

Module names don't map one-to-one; the C++ side folds several Python modules
into `LXMRouter` and adds a `MessageStore` with no Python counterpart.

| Python module | Our surface | Present | Evidence | Residual gap |
|---|---|---|---|---|
| `LXMessage.py` | `LXMessage.{h,cpp}` | yes | `lxmf-conformance` + `thicket-interop` (`run_lxmf_inbound.sh`) | Covered by payload-format, direct & attachment suites. Our scenario additionally asserts timestamp, title, content, field count, field msgpack wire bytes, source hash & signature validation on an inbound message **at our own microReticulum pin**, see the caveat below. |
| `LXMRouter.py` | `LXMRouter.{h,cpp}` | yes | `lxmf-conformance` + `thicket-interop` (`run_lxmf_inbound.sh`) | Covered by direct, opportunistic, dedup, combined suites. Our scenario covers the OPPORTUNISTIC inbound path only. DIRECT (over a Link) is **not** covered by us and is affected by divergence 7. |
| `LXStamper.py` | `LXStamper.{h,cpp}` | yes | partial `lxmf-conformance` | **Inbound stamp cost is a known bridge gap, the 2 skipped tests.** |
| `Handlers.py` | folded into `LXMRouter`, `PropagationNodeManager` | yes | `lxmf-conformance` (announce suites) | No separate handler surface to assess. |
| `LXMPeer.py` | `PropagationNodeManager`, `LXMRouter` | yes | `none` | Propagation suite is **skipped in upstream CI** (Resource transfer to `lxmd` doesn't conclude). |
| `LXMF.py` (constants) | `Type.h` and per-file constants | yes | `lxmf-conformance` | Payload-format suite exercises the wire constants. |
| n/a | `MessageStore.{h,cpp}` | C++ only | `none` | **No Python counterpart**, so there's nothing to be at parity *with*: Python keeps messages on the filesystem with no fixed pool. ⚠ **There is no MessageStore unit test.** It's exercised on hardware (attached, saving inbound and outbound) but that's device evidence, not parity evidence. See the capacity notes below. |

## Where we know we diverge

An unexplained divergence is indistinguishable from a bug.

1. **Transport store initialisation.** We initialise the path table, known
   destinations & packet hashlist regardless of `enable_transport`. Python
   splits *having* the structure from *restoring* it; microStore can't express
   that split. Proposed upstream.
2. **`MessageStore` fixed pools.** An embedded design with no Python analogue, the reference assumes storage we don't have. We ship **16 conversations × 64
   messages** with a 16-message hot tier, measured at 37,384 B, and held in
   static storage rather than allocated, because the allocator is redirected
   into a fixed pool and a `new` would spend the pool instead.
3. **Hot tier below the hard cap is rejected at compile time.** A hot count at or
   above the cap silently disables the archive tier; we now `static_assert`
   against it. Proposed upstream.
4. **`MessageStore` takes an optional codec.** `set_codec(encode, decode)`,
   unset by default, applied to every file the store persists. We install
   AES-256-CTR + HMAC-SHA256 keyed from the device identity. Nothing about the
   default path changes: with no codec the bytes written are identical. Sizes
   stay in decoded units so the store's own write-then-verify-readback still
   compares like with like, and a failed decode is reported as a failed read,
   because a file that doesn't authenticate is a corrupt file as far as the
   store is concerned. Proposed upstream. It's configurability with defaults
   unchanged, which is the shape that belongs there rather than here.
5. **Oversized persisted index truncates rather than wipes.** Reopening a store
   written by a build with larger limits drops the least recently active
   conversations and the oldest messages instead of clearing everything.
   Proposed upstream.

**Divergences 6 to 8 were found by reading function bodies rather than symbol
names, while building `test_interop/`.** Divergences 6 and 8 are each held by a
strict expected failure, so fixing one upstream turns a scenario red instead of
passing quietly. Divergence 7 has nothing against it.

6. **No Link watchdog.** `Link::start_watchdog()` (Link.cpp:884) has an empty
   body; `Link::__watchdog_job()` is inside a `/*p TODO */` comment block and
   isn't compiled. `Link::send_keepalive()` is compiled but has no caller. So a
   C++ link initiator never sends keepalives, a PENDING link never times out,
   and an ACTIVE link never goes STALE, a link survives the peer vanishing for
   as long as the process runs. Answering an inbound keepalive *does* work
   (Link.cpp:1455-1460). Demonstrated by `run_link_inbound.sh`, which pins it as
   a strict expected failure so implementing the watchdog turns the scenario red.
7. **Link proof validation is disabled.** `PacketReceipt::validate_link_proof`
   (Packet.cpp:907) is `//z if (link.validate(...))` followed by `if (false) {`,
   so a receipt for a packet sent over a Link never reaches DELIVERED and its
   delivery callback never fires. This is the same bug microLXMF's conformance
   work fixed in torlando-tech's microReticulum; our fork descends from
   attermann's and doesn't carry the fix. Not yet covered by a scenario.
8. **`Cryptography::hkdf()` ignores its `context` argument.**
   `Cryptography/HKDF.cpp` calls `HKDFCommon::extract(out, len)` and never
   passes `context`, though the underlying API accepts
   `extract(out, outLen, info, infoLen)`. No packet is affected today, `get_context()` returns empty on both sides, but it's a silently wrong
   public function. Pinned as a strict expected failure in
   `run_identity_vectors.sh`.

**`platformio.ini` understates the microReticulum fork.** Its comment lists
four changes, all of them Transport and path-store work, and names none of the
crypto fixes. X25519
clamping on key import sits in the pinned tree at
`Cryptography/X25519.h`, carrying a comment that explains it, and it is
load-bearing. Reverting it makes `run_identity_vectors.sh` fail four checks
including decryption of a reference ciphertext, while `run_cold_inbound.sh`
still passes, because a *generated* key is clamped by `Curve25519::dh1()` and
only an *imported* one (i.e. an identity reloaded from flash on boot) isn't.

## Hardware evidence

What has actually executed on a RAK4631, separated by how well we know it.
None of this populates a matrix row: those rows are parity against the Python
reference, and everything here is either our stack alone or our stack talking
to another microReticulum-family peer.

### Verified: read off the board on 2026-08-03

Boot log captured over USB serial, `wiscore_rak4631-noflash`, no RAK15001
fitted. Captured directly from the device.

- Radio: `LoRa init succeeded`, SX1262 online, continuous receive.
  914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm.
- Reticulum: `Transport starting...`, then `Transport mode is disabled`, running as a leaf, which is the shipped configuration.
- LXMF: router initialised, delivery destination registered, display name set.
- Announce: `Announce sent successfully`.
- Memory with the full stack up, on the 96 KB RNS pool we ship: 120,964 B of
  heap in use, 48,360 B of pool free, 116,040 B of system heap spare. The same
  measurement against a 64 KB pool read `Total SRAM 210104 B, Free SRAM
  131984 B`.
- Path-table index cost, measured with a gated probe: 52.0 B/record steady
  state, ~65 B including allocator overhead. Recorded in full with the design
  notes for that work, privately.

**Encrypted message storage, 2026-08-05.** `store round trip verified on this
filesystem (encrypt, write, read, decrypt)` at bring-up, then a real exchange:
`conversations=1 saved_in=1 saved_out=1` with the reply delivered under proof.
Inbound & outbound both stored encrypted, verified on hardware. This populates no matrix
row. There's no Python counterpart to a fixed-pool message store, but it is
the first hardware evidence the encryption paths execute on this silicon at
all; everything before it was a host result.

### Interoperation with the Python reference over LoRa: 2026-08-05, on hardware

**The first time this stack and the reference implementation have exchanged
messages over a radio.** Peer: `rnsd` 1.4.2 with LXMF, running on a Raspberry Pi
with an RNode as its transport, a full Python node, not another member of our
own lineage. Versions read on the Pi at run time, not copied from a document.

| | |
|---|---|
| Inbound | **DIRECT, over an RNS Link.** 146-byte message unpacked, source identity resolved, signature validated, delivery proof returned over the link |
| Outbound | **OPPORTUNISTIC.** 143-byte auto-reply composed, signed & encrypted on the nRF; `DELIVERED (proof received)` |
| Storage | both messages stored **encrypted**, `conversations=1 saved_in=1 saved_out=1` |
| Link | RSSI −73 dBm, SNR 12.75 dB, 914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm |
| Board | RAK4631, internal-flash bring-up environment, USB power **(not battery)** |

Both LXMF delivery methods, DIRECT inbound and OPPORTUNISTIC outbound,
therefore work against the reference in the direction that matters for a
handheld, being reached.

**Repeated on battery, untethered, 2026-08-05, reported rather than captured.** The same exchange with
NomadNet on the Pi, with the board disconnected from USB & running from its
battery: a message was delivered and the device's reply came back. **Reported from the
bench, observed in NomadNet, not independently captured**. Untethered
means no serial log, which is the point of the run and the limit of its
evidence. The board was still carrying the diagnostic build
(`-DTHICKET_LOG_DEBUG -DRNS_LOG_LEVEL=7`), which is heavier than the shipping
image; no power figure is claimed or implied by this.

⚠ **What this does not close.** The environment regenerates its identity every
boot, so it says nothing about state surviving a power cycle.

*(A phone client on the far end is deliberately **not** listed as a gap. The
peer was a Pi, but Sideband and the other phone clients bundle the same Python
RNS & LXMF this exchange already ran against, so a phone would test the
transport chain rather than this stack. The one genuine client-side risk, a peer configured to require message stamps, which drops unstamped messages
silently, is independent of form factor and is tracked in the LXStamper row
above.)*

**A false lead worth recording, because it cost an afternoon.** For twenty
minutes the reference established links to us & abandoned each one after ~14
seconds without sending anything. It looked exactly like a broken inbound Link
path, and this page already warned that DIRECT was the one delivery method we
had never exercised, which made the wrong explanation the attractive one. It
wasn't that. The sending side had **loaded a path entry for our destination from
storage** and kept answering its own client's path requests from that cache while
the radio path wasn't usable; it recovered only when a fresh announce arrived
directly over LoRa & replaced the entry. Its own log named it:
*"Trying to rediscover path … since an attempted local client link was never
established."*
Two lessons, both cheap next time: **a stale cached path presents as a protocol
incompatibility**, and the peer's log settles in one line what ours can't settle
at all.

### Observed at the bench: a round trip, not independently captured

Reported, not independently captured: the firmware has exchanged LXMF messages
with a **T-Deck running
pyxis**, using the auto-reply path: an inbound message is received, decrypted,
and a reply is composed, signed & encrypted **on the nRF** before going back
over LoRa.

This is the strongest evidence the project has that the on-device half of the
bring-up goal works, and it's stronger than a canned response would be, because composition
happens on the device.

**Established, from the log reproduced in `README.md` "Status":**

- **2026-08-03**, on `wiscore_rak4631-internalfs`, the round trip needed a
  filesystem `Identity::remember()` could actually write to, which `-noflash`
  doesn't provide.
- **Untethered.** The README records "Nothing was tethered", which speaks to
  our own requirement that the board run on battery with nothing tethered.
- **The reply was delivered, not merely sent**: `LXMF: DELIVERED (proof
  received)`. A delivery proof is cryptographic, so this is a stronger result
  than an unacknowledged transmit.

**Still unknown:**

- **Whether identity survived a power cycle.** Both bring-up environments
  regenerate the identity every boot, so this run can't have shown it. Persistence across a
  power cycle is untouched by it and still needs external flash.
- **Which pins.** The run predates the 2026-08-03 path-table pin bumps, so
  it's evidence about the earlier microReticulum & microStore pins rather than
  what's shipping now.

### What this doesn't show

Pyxis is built on microReticulum. A successful exchange therefore shows our
stack interoperating with **the same lineage**, not with the reference.
Interoperating with the reference implementation is a different question, which
is why a Python-lineage client is named specifically in our own done-condition.

✅ **That question is now answered separately**, see "Interoperation with the
Python reference over LoRa" above, 2026-08-05. This section is kept because the
distinction it draws is the reason that run was worth doing, and because a
same-lineage result should never again be filed as evidence of conformance.

## What would most improve this page

In rough order of value per effort:

1. **Any *matrix row* on real hardware.** Every row above is a host result. The
   stack itself has run on a RAK4631, see "Hardware evidence", but no parity
   scenario has, and that is the gap.
2. **A scenario for DIRECT (over-Link) LXMF delivery**, which is where
   divergence 7 bites and which our LXMF scenario doesn't reach.
3. **A pin against the IFAC gap.** Of the three still-open findings from the
   2026-06-09 assessment, the watchdog is held by an expected failure and
   proof validation is covered by item 2. IFAC has neither, and nothing
   planned.

## A note on stale assessments

The community wiki's microReticulum entry cites a conformance assessment dated
2026-06-09. **Three of its five high-priority findings are still open** in the
tree we ship. Checked at source with the file and line for each, in
`wet-bulb/microReticulum @ 9fb4828` (forked from `attermann @ 40fa6288`), and
re-verified 2026-09-06:

| 2026-06-09 finding | at our pin |
|---|---|
| Link watchdog as commented pseudocode | **open.** `Link::start_watchdog()` is three `//z` comment lines and an empty body (Link.cpp:884-888); `__watchdog_job()` sits inside `/*p TODO */` (Link.cpp:892-960) |
| proof validation behind `if (false)` | **open.** `//z if (link.validate(...))` followed by `if (false) {`, Packet.cpp:906-907 |
| IFAC commented out | **open.** Transmit path inside `/*p ... */`, Transport.cpp:1111-1145 |
| empty `Resource::cancel()` | closed, implemented, Resource.cpp |
| request handlers commented out | closed, implemented, Destination.cpp:369-391 |

**Only the watchdog is held**, by the expected-failure pin in
`run_link_inbound.sh`, divergence 6 above. Proof validation and IFAC have
nothing against them, so either could close upstream or widen further and
every scenario would stay green.

**Presence of a symbol is the weakest evidence there is.** `grep` for
`start_watchdog`, `send_keepalive` or an `ifac` branch finds all three, and all
three look present. Only reading the *body* shows that one is empty, one has no
caller, and one is inside a comment block. Read bodies, not symbol lists, and
not the driver names either.
