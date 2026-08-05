# EncryptedStore (vendored)

Authenticated encryption at rest for stored messages, keyed from the Reticulum
identity's own X25519 private key. Not written here.

## Origin

| | |
|---|---|
| Upstream | [konsumer/arduino-rns-encrypted-store](https://github.com/konsumer/arduino-rns-encrypted-store) |
| Commit | `5ccdc6577ba4abff99d5ea5d74a969388491d0bf` |
| Authored | 2026-08-02 by David Konsumer |
| Vendored | 2026-08-05 |
| License | MIT — full text in `./LICENSE`, copied from the upstream repo root |
| Copyright | © 2026 David Konsumer |

The same code is also proposed upstream as
[attermann/microReticulum#44](https://github.com/attermann/microReticulum/pull/44)
(`lib/encrypted_store/`), open since 2026-04-07 and unmerged. We take the
standalone repository rather than the PR, so nothing here waits on that review.

> ⚠ **The upstream licence is stated twice and the two do not agree.** The
> repository's `LICENSE` file is MIT and GitHub's API reports `MIT`
> **[V, `gh api repos/konsumer/arduino-rns-encrypted-store`, 2026-08-05]**, but
> the same commit's `library.json` carries `"license": "Apache-2.0"` — plausibly
> a leftover from the PR, where this code sits inside the Apache-2.0
> microReticulum tree.
>
> **This does not block us.** MIT and Apache-2.0 are both inbound-compatible
> with our GPL-3.0-or-later firmware, so the gate is satisfied under either
> reading. It is recorded because an ambiguous grant is worth resolving before
> it matters, and resolving it means asking the author — **founder's voice, not
> an agent's.**

## What it does

| | |
|---|---|
| Key derivation | HKDF-SHA256, 64 bytes, from the identity's X25519 private key, salted with the identity hash |
| Confidentiality | AES-256-CTR with a random 16-byte IV per file |
| Authentication | HMAC-SHA256 over version ‖ IV ‖ ciphertext, compared in constant time **before** any plaintext is produced |
| File layout | `version(1) ‖ IV(16) ‖ ciphertext(N) ‖ HMAC(32)` — 49 bytes of overhead |
| User input | **none** |

The keys derive from the identity, so **destroying the identity makes every
stored message unreadable at once** — crypto-erase for the cost of deleting one
key, with no flash wipe. On a sealed handheld that may need to be made safe in a
hurry, that property is arguably worth more than the encryption itself.

**What it does not do.** Our identity is still stored in the clear
(`src/main.cpp`). An attacker who images the whole flash reads the identity and
then derives these message keys from it. This defends a message store that
leaks *without* the identity, and it buys crypto-erase. Whole-flash compromise
needs the other half of the upstream work — the passphrase-protected identity
vault — which is not adopted here because it needs a passphrase-entry design
this device does not have yet.

## Local changes

Kept to the minimum, each marked in place with `THICKET:`.

1. **`#include <Arduino.h>` dropped from the header.** The library targets
   Arduino only; we also build this on the host for the conformance scenario,
   where that header does not exist. Nothing in the file used it — the Arduino
   types it would supply are not referenced.

Nothing else is changed. The key derivation, the file layout, the constant-time
compare and the HMAC-before-decrypt ordering are upstream's, untouched, because
those are the parts that have to stay bit-compatible with other readers of the
same format.

## Verified against the author's own vector

`test_interop/encstore_vectors/` decrypts a file **we did not produce**: the
92-byte `test/msgs/last.enc` shipped in upstream's `identity_vault` example,
together with the identity it was written for. Round-tripping our own output
proves only that we agree with ourselves; decrypting someone else's ciphertext
is what proves the format.
