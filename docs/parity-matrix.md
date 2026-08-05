# Parity matrix — what we ship, against the Python reference

The Reticulum manual is explicit that the protocol is defined "entirely and
authoritatively" by the Python reference implementation and the manual, and that
an implementation without full interoperability and sufficient functional parity
**is not Reticulum**. This page is our attempt to say, honestly and checkably,
where the stack we ship stands against that bar.

> [!IMPORTANT]
> **This is a coverage map, not a parity claim.** A row with evidence means a
> scenario exercised that surface against the reference — not that the module is
> at parity. **Read the Evidence and Residual gap columns, not the Present
> column.**
>
> ⚠ **Corrected 2026-08-04.** This said "most rows below say we have no
> evidence", which was true when written and is not now: of the eleven Reticulum
> rows, six carry evidence, one says `none`, and four are `absent` — meaning no
> counterpart exists to compare, which is a different statement from untested.
> The page understated the project for the second time; the earlier instance is
> noted above.

**Scope.** Thicket is firmware, not a Reticulum implementation. What is assessed
here is the stack we pin and ship: **microReticulum** (C++ RNS) and
**microLXMF** (C++ LXMF). Pins are in `platformio.ini`.

**No row in the matrix below has been run on hardware.** Every result in the
table comes from CI on x86 Linux hosts.

⚠ **Corrected 2026-08-03.** This section previously read "A RAK4631 has not yet
executed any of it", which is no longer true and was understating the project.
The stack has run on a RAK4631, and a message composed on the device has
reached a peer over LoRa. That does not populate any row below — those rows are
about parity against the *reference* implementation, which is a different
claim — but the blanket statement was wrong and is corrected here rather than
quietly deleted. See "Hardware evidence" below.

## What this result is about — the pins it passed against

**A pin bump invalidates the interop claim until the suite is re-run and the
version it passed against is recorded.** Until 2026-08-04 nothing
recorded it, so every green run was a claim about unnamed code.

**Last full pass: 2026-08-04, all 6 scenarios, at:**

| | |
|---|---|
| microReticulum | `9fb4828acdd24ff1e10ec528c2d24e9cae0e8acb` |
| microStore | `c5697b85a156e1b18372d7a190136bfd2c379545` |
| microLXMF | `b3d7f6cdd1989d6ad0027afdb31b7d8d0d672b78` |
| Python `rns` | 1.4.2 |
| Python `lxmf` | 1.1.1 |

`run_all.sh` now prints this block itself, resolved from `platformio.ini` and the
installed reference, so the record cannot be forgotten separately from the run.
**If the table above disagrees with a fresh run, the table is the stale one.**

## Evidence vocabulary

Deliberately narrow, so that a claim cannot be made by accident.

| Token | Means |
|---|---|
| `interop` | Exercised by one of the four microReticulum interop scenarios against **Python RNS 1.4.2**, in CI, on every push. |
| `thicket-interop` | Exercised by a scenario in **`test_interop/`** in this repo, against the official reference at pinned versions — **`rns==1.4.2` and `markqvist/lxmf==1.1.1`** — built on the microReticulum SHA we actually pin. Runs in CI on every push as the `thicket-interop` job. The Evidence column names the scenario. Every one of these has been shown to fail when the behaviour it tests is broken; see `test_interop/README.md`. |
| `lxmf-conformance` | Exercised by microLXMF's cross-implementation suite against the Python LXMF reference: **84 passed, 2 skipped**, run at both stock and our pool sizes. ⚠ **That suite builds microLXMF against `torlando-tech/microReticulum @ 6054f6ba`, not the fork we pin** (`conformance-bridge/CMakeLists.txt:57-64`, verified 2026-08-03). It is evidence about the LXMF layer; it is not evidence about the LXMF layer on our RNS layer. `thicket-interop` is. |
| `unit` | Covered by a native unit test in this repo. |
| `none` | **Code exists; we have not verified it against the reference.** Not a claim of absence, a claim of ignorance. |
| `absent` | No counterpart in the C++ stack at the pins we ship. |

⚠ **The largest caveat: direction — and it is narrower than first stated.**
**Corrected 2026-08-03** after reading the receivers rather than the driver
names. What is actually covered:

| scenario | direction |
|---|---|
| packet | **round trip.** C++ sends, Python replies, C++ receives and asserts payload match |
| request | C++ receives a **response** from a Python-side `/echo` handler |
| link | C++ → Python only |
| resource | C++ → Python only |

So inbound decoding is not wholly untested. **The real gap is that no scenario
has Python originate cold.** In every case the C++ side speaks first and Python
answers. Nothing tests a Python peer initiating to a device that has not just
transmitted — which is precisely what a handheld does all day.

**Update 2026-08-03: that gap is now closed for packets, LXMF delivery and
links.** `test_interop/` in this repo adds scenarios in which the Python side
originates:

| scenario | direction |
|---|---|
| `run_cold_inbound.sh` | **Python originates cold.** The C++ side has announced and has never transmitted to the peer; the peer never announces at all, so the C++ side cannot address it. |
| `run_lxmf_inbound.sh` | Python LXMF originates to our `lxmf.delivery`; C++ asserts the decoded fields. Not cold — LXMF signature validation needs the source identity, so the peer announces first. |
| `run_link_inbound.sh` | **Python establishes the Link**, the C++ side only responds. |
| `run_identity_vectors.sh` | No direction; fixed reference vectors. |

## Reticulum — Python `RNS` 1.4.2 → microReticulum

| Python module | Our surface | Present | Evidence | Residual gap |
|---|---|---|---|---|
| `Packet.py` | `Packet.cpp` | yes | `interop` + `thicket-interop` (`run_cold_inbound.sh`) | Cold inbound now verified: a Python peer originates 383 bytes (`ENCRYPTED_MDU`) to a destination that has only announced, and the C++ side decrypts, validates, and returns a proof the reference accepts. |
| `Link.py` | `Link.cpp` | yes | `interop` + `thicket-interop` (`run_link_inbound.sh`) | Python-initiated establish, data round trip, and keepalive **response** now verified; the reference's watchdog closes the link with `TIMEOUT` when the wire is cut. **But microReticulum has no Link watchdog of its own** — see divergence 6 below — so it never originates keepalives and never times a link out. Link *proof* validation is also disabled; see divergence 7. |
| `Resource.py` | `Resource.{h,cpp}` | yes | `interop` | Transfer scenario passes host-side. Note microLXMF's own docs report Resource transfer to `lxmd` not concluding. |
| `Destination.py` | `Destination.cpp` | yes | `interop` (request/response) | GROUP destinations unverified. |
| `Identity.py` | `Identity.cpp` | yes | `thicket-interop` (`run_identity_vectors.sh`) | Key derivation from an imported private key, identity and destination hashing, `full_hash`/`truncated_hash`, HKDF, deterministic Ed25519 signing, signature validation (including two negative cases) and decryption of a reference-produced ciphertext all match Python RNS 1.4.2. One divergence found: `Cryptography::hkdf()` ignores its `context` argument — see divergence 8. |
| `Transport.py` | `Transport.cpp` | yes | `thicket-interop` (`run_multihop_inbound.sh`, `run_transport_forward.sh`) | Covered in both roles as of 2026-08-04. **As a leaf:** a transport-enabled reference node sits between the originator and us; the path is learned from a relayed announce, the packet arrives with a non-zero hop count, and the proof returns across the relay. **As a router:** two reference peers on segments with no member in common except us reach each other through us, arriving at `hops=2`. They cannot hear each other directly, so delivery is proof of forwarding. This is the only scenario running `transport_enabled(true)`, the mode our four local patches affect. **Residual: the peer is always the reference, never a second microReticulum.** Two of our own nodes would exercise constructs we only ever *produce* — our announces through our own parser, our forwarding of our own packets — which no scenario here reaches. It would **not** be evidence of conformance: two implementations that misread the protocol identically agree with each other perfectly. |
| `Reticulum.py` | `Reticulum.cpp` | yes | `none` | Config surface differs by construction; not assessed. |
| `Channel.py` | `Channel.{h,cpp}` | **files only** | `absent` in practice | **Corrected 2026-08-03.** The files exist but there is no implementation: `Channel.cpp` is 26 lines of `#include`, `Link::get_channel()` is commented out (Link.cpp:1136) and the `CHANNEL` packet-context branch of `Link::receive` is commented out (Link.cpp:1489). There is no API to open a channel, so a scenario cannot be written. Previously listed as Present `yes` / `none`, which read as "untested" rather than "not there". |
| `Buffer.py` | — | **absent** | `absent` | No counterpart at our pin. |
| `Discovery.py` | — | **absent** | `absent` | No counterpart at our pin. |
| `Resolver.py` | — | **absent** | `absent` | No counterpart at our pin. |

**[verified 2026-08-03 — module lists read from `markqvist/Reticulum` at tag
`1.4.2` and from our pinned microReticulum tree, headers included.]**

## LXMF — Python `LXMF` → microLXMF

Module names do not map one-to-one; the C++ side folds several Python modules
into `LXMRouter` and adds a `MessageStore` with no Python counterpart.

| Python module | Our surface | Present | Evidence | Residual gap |
|---|---|---|---|---|
| `LXMessage.py` | `LXMessage.{h,cpp}` | yes | `lxmf-conformance` + `thicket-interop` (`run_lxmf_inbound.sh`) | Covered by payload-format, direct and attachment suites. Our scenario additionally asserts timestamp, title, content, field count, field msgpack wire bytes, source hash and signature validation on an inbound message **at our own microReticulum pin** — see the caveat below. |
| `LXMRouter.py` | `LXMRouter.{h,cpp}` | yes | `lxmf-conformance` + `thicket-interop` (`run_lxmf_inbound.sh`) | Covered by direct, opportunistic, dedup, combined suites. Our scenario covers the OPPORTUNISTIC inbound path only. DIRECT (over a Link) is **not** covered by us and is affected by divergence 7. |
| `LXStamper.py` | `LXStamper.{h,cpp}` | yes | partial `lxmf-conformance` | **Inbound stamp cost is a known bridge gap — the 2 skipped tests.** |
| `Handlers.py` | folded into `LXMRouter`, `PropagationNodeManager` | yes | `lxmf-conformance` (announce suites) | No separate handler surface to assess. |
| `LXMPeer.py` | `PropagationNodeManager`, `LXMRouter` | yes | `none` | Propagation suite is **skipped in upstream CI** (Resource transfer to `lxmd` does not conclude). |
| `LXMF.py` (constants) | `Type.h` and per-file constants | yes | `lxmf-conformance` | Payload-format suite exercises the wire constants. |
| — | `MessageStore.{h,cpp}` | C++ only | `none` | **No Python counterpart**, so there is nothing to be at parity *with*: Python keeps messages on the filesystem with no fixed pool. ⚠ **This row said `unit` until 2026-08-04 and there is no MessageStore unit test** — an evidence claim with nothing behind it, in the one document that exists to prevent those. It is exercised on hardware (attached, saving inbound and outbound) but that is device evidence, not parity evidence. See the capacity notes below. |

## Where we know we diverge

Stated because an unexplained divergence is indistinguishable from a bug.

1. **Transport store initialisation.** We initialise the path table, known
   destinations and packet hashlist regardless of `enable_transport`. Python
   splits *having* the structure from *restoring* it; microStore cannot express
   that split. Proposed upstream.
2. **`MessageStore` fixed pools.** An embedded design with no Python analogue —
   the reference assumes storage we do not have. We ship **16 conversations × 64
   messages** with a 16-message hot tier — measured at 37,384 B, and held in
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
   because a file that does not authenticate is a corrupt file as far as the
   store is concerned. Proposed upstream — it is configurability with defaults
   unchanged, which is the shape that belongs there rather than here.
5. **Oversized persisted index truncates rather than wipes.** Reopening a store
   written by a build with larger limits drops the least recently active
   conversations and the oldest messages instead of clearing everything.
   Proposed upstream.

**Added 2026-08-03, found while building `test_interop/`. Numbers 5-7 are
places the stack diverges from the reference that nobody had written down.**

6. **No Link watchdog.** `Link::start_watchdog()` (Link.cpp:884) has an empty
   body; `Link::__watchdog_job()` is inside a `/*p TODO */` comment block and is
   not compiled. `Link::send_keepalive()` is compiled but has no caller. So a
   C++ link initiator never sends keepalives, a PENDING link never times out,
   and an ACTIVE link never goes STALE — a link survives the peer vanishing for
   as long as the process runs. Answering an inbound keepalive *does* work
   (Link.cpp:1455-1460). Demonstrated by `run_link_inbound.sh`, which pins it as
   a strict expected failure so implementing the watchdog turns the scenario red.
7. **Link proof validation is disabled.** `PacketReceipt::validate_link_proof`
   (Packet.cpp:907) is `//z if (link.validate(...))` followed by `if (false) {`,
   so a receipt for a packet sent over a Link never reaches DELIVERED and its
   delivery callback never fires. This is the same bug microLXMF's conformance
   work fixed in torlando-tech's microReticulum; our fork descends from
   attermann's and does not carry the fix. Not yet covered by a scenario.
8. **`Cryptography::hkdf()` ignores its `context` argument.**
   `Cryptography/HKDF.cpp` calls `HKDFCommon::extract(out, len)` and never
   passes `context`, though the underlying API accepts
   `extract(out, outLen, info, infoLen)`. No packet is affected today —
   `get_context()` returns empty on both sides — but it is a silently wrong
   public function. Pinned as a strict expected failure in
   `run_identity_vectors.sh`.

**Also worth recording: `platformio.ini` understates our microReticulum fork.**
Its comment says the fork "carries exactly one change" (divergence 1). It
carries four: `git log 40fa6288..0fb6151` also shows a PKCS#7 padding fix, an
HMAC double-feed fix, and X25519 clamping on key import. The last of these is
load-bearing — reverting it makes `run_identity_vectors.sh` fail four checks
including decryption of a reference ciphertext, while `run_cold_inbound.sh`
still passes, because a *generated* key is clamped by `Curve25519::dh1()` and
only an *imported* one (i.e. an identity reloaded from flash on boot) is not.

## Hardware evidence

What has actually executed on a RAK4631, separated by how well we know it.
None of this populates a matrix row: those rows are parity against the Python
reference, and everything here is either our stack alone or our stack talking
to another microReticulum-family peer.

### Verified — read off the board on 2026-08-03

Boot log captured over USB serial, `wiscore_rak4631-noflash`, no RAK15001
fitted. **[V]**

- Radio: `LoRa init succeeded`, SX1262 online, continuous receive.
  914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm.
- Reticulum: `Transport starting...`, then `Transport mode is disabled` —
  running as a leaf, which is the shipped configuration.
- LXMF: router initialised, delivery destination registered, display name set.
- Announce: `Announce sent successfully`.
- Memory with the full stack up: `Total SRAM 210104 B, Free SRAM 131984 B`,
  on a 64 KB RNS pool. **Superseded 2026-08-04**: the pool is now 96 KB and the
  same measurement reads 120,964 B of heap in use, 48,360 B of pool free and
  116,040 B of system heap spare. The 64 KB figure is kept because it is what
  was read on the day.
- Path-table index cost, measured with a gated probe: 52.0 B/record steady
  state, ~65 B including allocator overhead. Recorded in full with the design
  notes for that work, privately.

**Encrypted message storage, 2026-08-05.** `store round trip verified on this
filesystem (encrypt, write, read, decrypt)` at bring-up, then a real exchange:
`conversations=1 saved_in=1 saved_out=1` with the reply delivered under proof.
Inbound and outbound both stored encrypted. **[V-hw]** This populates no matrix
row — there is no Python counterpart to a fixed-pool message store — but it is
the first hardware evidence the encryption paths execute on this silicon at
all; everything before it was a host result.

### Interoperation with the Python reference over LoRa — 2026-08-05 **[V-hw]**

**The first time this stack and the reference implementation have exchanged
messages over a radio.** Peer: `rnsd` 1.4.2 with LXMF, running on a Raspberry Pi
with an RNode as its transport — a full Python node, not another member of our
own lineage. Versions read on the Pi at run time, not copied from a document.

| | |
|---|---|
| Inbound | **DIRECT, over an RNS Link.** 146-byte message unpacked, source identity resolved, signature validated, delivery proof returned over the link |
| Outbound | **OPPORTUNISTIC.** 143-byte auto-reply composed, signed and encrypted on the nRF; `DELIVERED (proof received)` |
| Storage | both messages stored **encrypted** — `conversations=1 saved_in=1 saved_out=1` |
| Link | RSSI −73 dBm, SNR 12.75 dB, 914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm |
| Board | RAK4631, internal-flash bring-up environment, USB power **(not battery)** |

Both LXMF delivery methods therefore work against the reference, in the
direction that matters for a handheld — being reached.

**Repeated on battery, untethered — 2026-08-05 [R].** The same exchange with
NomadNet on the Pi, with the board disconnected from USB and running from its
battery: a message was delivered and the device's reply came back. **Reported by
the founder, observed in NomadNet, not independently captured** — untethered
means no serial log, which is the point of the run and the limit of its
evidence. The board was still carrying the diagnostic build
(`-DTHICKET_LOG_DEBUG -DRNS_LOG_LEVEL=7`), which is heavier than the shipping
image; no power figure is claimed or implied by this.

⚠ **What this does not close.** The environment regenerates its identity every
boot, so it says nothing about state surviving a power cycle.

*(A phone client on the far end is deliberately **not** listed as a gap. The
peer was a Pi, but Sideband and the other phone clients bundle the same Python
RNS and LXMF this exchange already ran against, so a phone would test the
transport chain rather than this stack. The one genuine client-side risk —
a peer configured to require message stamps, which drops unstamped messages
silently — is independent of form factor and is tracked in the LXStamper row
above.)*

**A false lead worth recording, because it cost an afternoon.** For twenty
minutes the reference established links to us and abandoned each one after ~14
seconds without sending anything. It looked exactly like a broken inbound Link
path — and this page already warned that DIRECT was the one delivery method we
had never exercised, which made the wrong explanation the attractive one. It was
not that. The sending side had **loaded a path entry for our destination from
storage** and kept answering its own client's path requests from that cache while
the radio path was not usable; it recovered only when a fresh announce arrived
directly over LoRa and replaced the entry. Its own log named it:
*"Trying to rediscover path … since an attempted local client link was never
established."*
Two lessons, both cheap next time: **a stale cached path presents as a protocol
incompatibility**, and the peer's log settles in one line what ours cannot settle
at all.

### Reported by the founder — a round trip, not independently observed

**[R]** The firmware has exchanged LXMF messages with a **T-Deck running
pyxis**, using the auto-reply path: an inbound message is received, decrypted,
and a reply is composed, signed and encrypted **on the nRF** before going back
over LoRa.

This is the strongest evidence the project has that the on-device half of the
bring-up goal works, and it is stronger than a canned response would be, because composition
happens on the device.

**Established, from the log reproduced in `README.md` "Status":**

- **2026-08-03**, on `wiscore_rak4631-internalfs` — the round trip needed a
  filesystem `Identity::remember()` could actually write to, which `-noflash`
  does not provide.
- **Untethered.** The README records "Nothing was tethered", which speaks to
  our own requirement that the board run on battery with nothing tethered.
- **The reply was delivered, not merely sent**: `LXMF: DELIVERED (proof
  received)`. A delivery proof is cryptographic, so this is a stronger result
  than an unacknowledged transmit.

**Still unknown:**

- **Whether identity survived a power cycle.** Both bring-up environments
  regenerate the identity every boot, so this run cannot have shown it. Persistence across a
  power cycle is untouched by it and still needs external flash.
- **Which pins.** The run predates the 2026-08-03 path-table pin bumps, so it
  is evidence about the earlier microReticulum and microStore pins rather than
  what is shipping now.

### What this does not show

Pyxis is built on microReticulum. A successful exchange therefore shows our
stack interoperating with **the same lineage**, not with the reference.
Interoperating with the reference implementation is a different question, which
is why a Python-lineage client is named specifically in our own done-condition.

✅ **That question is now answered separately** — see "Interoperation with the
Python reference over LoRa" above, 2026-08-05. This section is kept because the
distinction it draws is the reason that run was worth doing, and because a
same-lineage result should never again be filed as evidence of conformance.

## What would most improve this page

In rough order of value per effort:

1. ~~**A Python-sends / C++-receives scenario.**~~ Done 2026-08-03:
   `run_cold_inbound.sh`, `run_lxmf_inbound.sh`, `run_link_inbound.sh`.
2. ~~**Identity vectors.**~~ Done 2026-08-03: `run_identity_vectors.sh`.
3. ~~**Multi-hop transport.**~~ Done 2026-08-04: `run_multihop_inbound.sh`
   covers us as a leaf behind a relay, `run_transport_forward.sh` covers us as
   the router two reference peers reach each other through. **Residual: both
   scenarios put the reference on the other end.** Two of our own nodes talking
   to each other is still untested, and is the case that would catch a
   divergence both implementations share.
4. **Any *matrix row* on real hardware.** Every row above is a host result.
   The stack itself has run on a RAK4631 — see "Hardware evidence" — but no
   parity scenario has, and that is the gap.
5. **A scenario for DIRECT (over-Link) LXMF delivery**, which is where
   divergence 7 bites and which our LXMF scenario does not reach.

## A note on stale assessments

The community wiki's microReticulum entry cites a conformance assessment dated
2026-06-09 whose high-priority findings — Link watchdog as commented pseudocode,
proof validation behind `if (false)`, empty `Resource::cancel()`, request
handlers commented out, IFAC commented out — are **all closed** in the version we
pin **[verified 2026-08-03 against `0.5.0-8`]**. Presence of code is not parity,
which is why those rows carried no evidence at the time. But the assessment no
longer describes this stack.

> [!CAUTION]
> **The paragraph above is wrong, and this correction is the more reliable of
> the two.** Re-checked at source in the exact tree we pin
> (`wet-bulb/microReticulum @ 0fb6151`, forked from `attermann @ 40fa6288`),
> file and line noted for each, on 2026-08-03 while building `test_interop/`:
>
> | 2026-06-09 finding | claimed | actual at our pin |
> |---|---|---|
> | Link watchdog as commented pseudocode | closed | **STILL OPEN** — `Link::start_watchdog()` empty (Link.cpp:884); `__watchdog_job()` inside `/*p TODO */` (Link.cpp:892-960) |
> | proof validation behind `if (false)` | closed | **STILL OPEN** — `PacketReceipt::validate_link_proof`, Packet.cpp:907 |
> | empty `Resource::cancel()` | closed | closed — implemented, Resource.cpp |
> | request handlers commented out | closed | closed — implemented, Destination.cpp:369-391 |
> | IFAC commented out | closed | **STILL OPEN** — transmit path inside `/*p ... */`, Transport.cpp:1111-1145 |
>
> Three of the five are still open. The assessment describes this stack more
> accurately than we did. Two of the three now have scenarios or expected-failure
> pins against them (divergences 6 and 8 above); the IFAC one does not.
>
> How the earlier check went wrong is worth naming, because it is repeatable:
> `grep` for the *symbol* finds `start_watchdog`, `send_keepalive` and an `ifac`
> branch and they all look present. Only reading the *body* shows that one is
> empty, one has no caller, and one is inside a comment. Presence of a symbol is
> even weaker evidence than presence of code.
