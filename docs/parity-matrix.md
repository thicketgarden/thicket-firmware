# Parity matrix — what we ship, against the Python reference

The Reticulum manual is explicit that the protocol is defined "entirely and
authoritatively" by the Python reference implementation and the manual, and that
an implementation without full interoperability and sufficient functional parity
**is not Reticulum**. This page is our attempt to say, honestly and checkably,
where the stack we ship stands against that bar.

> [!IMPORTANT]
> **This is a coverage map, not a parity claim.** Most rows below say we have no
> evidence. That is the accurate state, and it is the reason the page exists.
> **Do not read a populated row as a passing row** — read the Evidence column.

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
| `Link.py` | `Link.cpp` | yes | `interop` + `thicket-interop` (`run_link_inbound.sh`) | Python-initiated establish, data round trip, and keepalive **response** now verified; the reference's watchdog closes the link with `TIMEOUT` when the wire is cut. **But microReticulum has no Link watchdog of its own** — see divergence 5 below — so it never originates keepalives and never times a link out. Link *proof* validation is also disabled; see divergence 6. |
| `Resource.py` | `Resource.{h,cpp}` | yes | `interop` | Transfer scenario passes host-side. Note microLXMF's own docs report Resource transfer to `lxmd` not concluding. |
| `Destination.py` | `Destination.cpp` | yes | `interop` (request/response) | GROUP destinations unverified. |
| `Identity.py` | `Identity.cpp` | yes | `thicket-interop` (`run_identity_vectors.sh`) | Key derivation from an imported private key, identity and destination hashing, `full_hash`/`truncated_hash`, HKDF, deterministic Ed25519 signing, signature validation (including two negative cases) and decryption of a reference-produced ciphertext all match Python RNS 1.4.2. One divergence found: `Cryptography::hkdf()` ignores its `context` argument — see divergence 7. |
| `Transport.py` | `Transport.cpp` | yes | `thicket-interop` (`run_multihop_inbound.sh`, `run_transport_forward.sh`) | **Both directions now covered, 2026-08-04.** As a *leaf*: a transport-enabled reference node sits between the originator and us, so the path is learned through a router, the packet arrives with a non-zero hop count, and the proof travels back across the relay. As a *router*: two reference peers sit on segments with no member in common except us and reach each other through us — proof of forwarding, since they cannot hear one another — arriving at `hops=2`, which is the signature of having crossed exactly one forwarding node. The forwarding scenario runs with `transport_enabled(true)`, the mode all four of our local patches are about and which nothing exercised until now. **Residual: the reference is always the peer, never a second microReticulum.** Two of our own nodes talking to each other is untested, and would catch a divergence both sides share. |
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
| `LXMRouter.py` | `LXMRouter.{h,cpp}` | yes | `lxmf-conformance` + `thicket-interop` (`run_lxmf_inbound.sh`) | Covered by direct, opportunistic, dedup, combined suites. Our scenario covers the OPPORTUNISTIC inbound path only. DIRECT (over a Link) is **not** covered by us and is affected by divergence 6. |
| `LXStamper.py` | `LXStamper.{h,cpp}` | yes | partial `lxmf-conformance` | **Inbound stamp cost is a known bridge gap — the 2 skipped tests.** |
| `Handlers.py` | folded into `LXMRouter`, `PropagationNodeManager` | yes | `lxmf-conformance` (announce suites) | No separate handler surface to assess. |
| `LXMPeer.py` | `PropagationNodeManager`, `LXMRouter` | yes | `none` | Propagation suite is **skipped in upstream CI** (Resource transfer to `lxmd` does not conclude). |
| `LXMF.py` (constants) | `Type.h` and per-file constants | yes | `lxmf-conformance` | Payload-format suite exercises the wire constants. |
| — | `MessageStore.{h,cpp}` | C++ only | `unit` | **No Python counterpart**: Python keeps messages on the filesystem with no fixed pool. See the capacity notes below. |

## Where we know we diverge

Stated because an unexplained divergence is indistinguishable from a bug.

1. **Transport store initialisation.** We initialise the path table, known
   destinations and packet hashlist regardless of `enable_transport`. Python
   splits *having* the structure from *restoring* it; microStore cannot express
   that split. Proposed upstream.
2. **`MessageStore` fixed pools.** An embedded design with no Python analogue —
   the reference assumes storage we do not have. We ship 8 conversations × 32
   messages with a 16-message hot tier.
3. **Hot tier below the hard cap is rejected at compile time.** A hot count at or
   above the cap silently disables the archive tier; we now `static_assert`
   against it. Proposed upstream.
4. **Oversized persisted index truncates rather than wipes.** Reopening a store
   written by a build with larger limits drops the least recently active
   conversations and the oldest messages instead of clearing everything.
   Proposed upstream.

**Added 2026-08-03, found while building `test_interop/`. Numbers 5-7 are
places the stack diverges from the reference that nobody had written down.**

5. **No Link watchdog.** `Link::start_watchdog()` (Link.cpp:884) has an empty
   body; `Link::__watchdog_job()` is inside a `/*p TODO */` comment block and is
   not compiled. `Link::send_keepalive()` is compiled but has no caller. So a
   C++ link initiator never sends keepalives, a PENDING link never times out,
   and an ACTIVE link never goes STALE — a link survives the peer vanishing for
   as long as the process runs. Answering an inbound keepalive *does* work
   (Link.cpp:1455-1460). Demonstrated by `run_link_inbound.sh`, which pins it as
   a strict expected failure so implementing the watchdog turns the scenario red.
6. **Link proof validation is disabled.** `PacketReceipt::validate_link_proof`
   (Packet.cpp:907) is `//z if (link.validate(...))` followed by `if (false) {`,
   so a receipt for a packet sent over a Link never reaches DELIVERED and its
   delivery callback never fires. This is the same bug microLXMF's conformance
   work fixed in torlando-tech's microReticulum; our fork descends from
   attermann's and does not carry the fix. Not yet covered by a scenario.
7. **`Cryptography::hkdf()` ignores its `context` argument.**
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
  on a 64 KB RNS pool.
- Path-table index cost, measured with a gated probe: 52.0 B/record steady
  state, ~65 B including allocator overhead. See
  recorded with the design notes for that work.

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
stack interoperating with **the same lineage**, not with the reference. Interoperating with the reference
implementation is a different question, which is why a Python-lineage client is
named specifically in our own done-condition, and it is what `test_interop/`
exists to answer.

## What would most improve this page

In rough order of value per effort:

1. ~~**A Python-sends / C++-receives scenario.**~~ Done 2026-08-03:
   `run_cold_inbound.sh`, `run_lxmf_inbound.sh`, `run_link_inbound.sh`.
2. ~~**Identity vectors.**~~ Done 2026-08-03: `run_identity_vectors.sh`.
3. **Multi-hop transport.** Currently unexercised in any form.
4. **Any *matrix row* on real hardware.** Every row above is a host result.
   The stack itself has run on a RAK4631 — see "Hardware evidence" — but no
   parity scenario has, and that is the gap.
5. **A scenario for DIRECT (over-Link) LXMF delivery**, which is where
   divergence 6 bites and which our LXMF scenario does not reach.

## A note on stale assessments

The community wiki's microReticulum entry cites a conformance assessment dated
2026-06-09 whose high-priority findings — Link watchdog as commented pseudocode,
proof validation behind `if (false)`, empty `Resource::cancel()`, request
handlers commented out, IFAC commented out — are **all closed** in the version we
pin **[verified 2026-08-03 against `0.5.0-8`]**. Presence of code is not parity,
which is why those rows above still read `none`. But the assessment no longer
describes this stack.

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
> pins against them (divergences 5 and 7 above); the IFAC one does not.
>
> How the earlier check went wrong is worth naming, because it is repeatable:
> `grep` for the *symbol* finds `start_watchdog`, `send_keepalive` and an `ifac`
> branch and they all look present. Only reading the *body* shows that one is
> empty, one has no caller, and one is inside a comment. Presence of a symbol is
> even weaker evidence than presence of code.
