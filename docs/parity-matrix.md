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

**Nothing here has been run on hardware.** Every result below comes from CI on
x86 Linux hosts. A RAK4631 has not yet executed any of it.

## Evidence vocabulary

Deliberately narrow, so that a claim cannot be made by accident.

| Token | Means |
|---|---|
| `interop` | Exercised by one of the four microReticulum interop scenarios against **Python RNS 1.4.2**, in CI, on every push. |
| `lxmf-conformance` | Exercised by microLXMF's cross-implementation suite against the Python LXMF reference: **84 passed, 2 skipped**, run at both stock and our pool sizes. |
| `unit` | Covered by a native unit test in this repo. |
| `none` | **Code exists; we have not verified it against the reference.** Not a claim of absence, a claim of ignorance. |
| `absent` | No counterpart in the C++ stack at the pins we ship. |

⚠ **The single largest caveat: direction.** All four interop scenarios are
**C++ sending, Python receiving**. The reverse path — Python sending, our stack
receiving and decoding — **is not covered by any scenario**. For a handheld whose
entire purpose is to receive messages, that is the gap that matters most.

## Reticulum — Python `RNS` 1.4.2 → microReticulum

| Python module | Our surface | Present | Evidence | Residual gap |
|---|---|---|---|---|
| `Packet.py` | `Packet.cpp` | yes | `interop` | Send path only. Receive/decode from a Python sender untested. |
| `Link.py` | `Link.cpp` | yes | `interop` | Lifecycle scenario covers establish and teardown; keepalive under real link loss untested. |
| `Resource.py` | `Resource.{h,cpp}` | yes | `interop` | Transfer scenario passes host-side. Note microLXMF's own docs report Resource transfer to `lxmd` not concluding. |
| `Destination.py` | `Destination.cpp` | yes | `interop` (request/response) | GROUP destinations unverified. |
| `Identity.py` | `Identity.cpp` | yes | `none` | Used by every scenario indirectly; never asserted against reference vectors. |
| `Transport.py` | `Transport.cpp` | yes | `none` | We carry a local patch here (store init regardless of `enable_transport`). Multi-hop routing untested by us. |
| `Reticulum.py` | `Reticulum.cpp` | yes | `none` | Config surface differs by construction; not assessed. |
| `Channel.py` | `Channel.{h,cpp}` | yes | `none` | Not exercised by any scenario we run. |
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
| `LXMessage.py` | `LXMessage.{h,cpp}` | yes | `lxmf-conformance` | Covered by payload-format, direct and attachment suites. |
| `LXMRouter.py` | `LXMRouter.{h,cpp}` | yes | `lxmf-conformance` | Covered by direct, opportunistic, dedup, combined suites. |
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

## What would most improve this page

In rough order of value per effort:

1. **A Python-sends / C++-receives scenario.** Closes the direction gap, which is
   the one that matters for a receiving device.
2. **Identity vectors.** Assert key derivation and signatures against known
   reference outputs rather than relying on scenarios passing.
3. **Multi-hop transport.** Currently unexercised in any form.
4. **Anything at all on real hardware.** Every row above is a host result.

## A note on stale assessments

The community wiki's microReticulum entry cites a conformance assessment dated
2026-06-09 whose high-priority findings — Link watchdog as commented pseudocode,
proof validation behind `if (false)`, empty `Resource::cancel()`, request
handlers commented out, IFAC commented out — are **all closed** in the version we
pin **[verified 2026-08-03 against `0.5.0-8`]**. Presence of code is not parity,
which is why those rows above still read `none`. But the assessment no longer
describes this stack.
