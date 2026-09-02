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
| License | MIT, full text in `./LICENSE`, copied from the upstream repo root |
| Copyright | © 2026 David Konsumer |

The same code is also proposed upstream as
[attermann/microReticulum#44](https://github.com/attermann/microReticulum/pull/44)
(`lib/encrypted_store/`), open since 2026-04-07 & unmerged. We take the
standalone repository rather than the PR, so nothing here waits on that review.

> **The upstream licence is stated twice and the two statements disagree. This
> project treats it as MIT.**
>
> The repository's `LICENSE` file is MIT and GitHub's API reports `MIT`
> (checked with `gh api repos/konsumer/arduino-rns-encrypted-store`, 2026-08-05). The
> same commit's `library.json` carries `"license": "Apache-2.0"`.
>
> MIT is the deliberate statement & Apache-2.0 is the leftover. The `LICENSE`
> file was added on 2026-08-02 in response to a question raised on the PR
> thread; the repositories carried no licence at all before that.
> `library.json` wasn't touched in that
> change and still reflects the file's origin inside the Apache-2.0
> microReticulum tree, where this code was first proposed.
>
> Nothing turns on it either way: both are inbound-compatible with our
> GPL-3.0-or-later firmware, so the gate is satisfied under either reading.
> Recorded rather than left implicit, because "we assumed" and "we checked and
> reasoned" are different provenance and only one of them survives review.

## What it does

| | |
|---|---|
| Key derivation | HKDF-SHA256, 64 bytes, from the identity's X25519 private key, salted with the identity hash |
| Confidentiality | AES-256-CTR with a random 16-byte IV per file |
| Authentication | HMAC-SHA256 over version ‖ IV ‖ ciphertext, compared in constant time **before** any plaintext is produced |
| File layout | `version(1) ‖ IV(16) ‖ ciphertext(N) ‖ HMAC(32)`, 49 bytes of overhead |
| User input | **none** |

The keys derive from the identity, so **destroying the identity makes every
stored message unreadable at once**, crypto-erase for the cost of deleting one
key, with no flash wipe. On a sealed handheld that may need to be made safe in a
hurry, that property is arguably worth more than the encryption itself.

**What it does not do.** Our identity is still stored in the clear
(`src/main.cpp`). An attacker who images the whole flash reads the identity and
then derives these message keys from it. This defends a message store that
leaks *without* the identity, and it buys crypto-erase. Whole-flash compromise
needs the other half of the upstream work, the passphrase-protected identity
vault, which isn't adopted here because it needs a passphrase-entry design
this device doesn't have yet.

## Why vendored rather than a `lib_deps` entry

Unlike `lib/LoRaInterface`, the "PlatformIO cannot depend on a subdirectory"
argument doesn't apply, upstream publishes this as a whole repository, so a
`lib_deps` URL would have been structurally fine. Three other things decided it.

**1. The includes are stale, and no fetch method fixes that.** Upstream was
written against microReticulum at `54c934e`, the base of PR #44, where `src/`
was flat: `src/Identity.h`, `src/Cryptography/HKDF.h`. Those includes were
correct on 2026-04-07. microReticulum has since restructured to
`src/microReticulum/…`, including attermann's own HEAD (checked 2026-08-05), so
this isn't a peculiarity of our fork. The library as published no longer
compiles against the dependency its own `library.json` declares. Its PR has had
no maintainer action since 2026-05-05, so nothing ever forced a rebase.

**2. Its dependencies are unpinned.** `library.json` lists `attermann/Crypto`
and `attermann/microStore` with no commit, and the README instructs adding
`attermann/microReticulum.git` bare. Every dependency in our `platformio.ini`
is pinned to an explicit SHA and `scripts/patch_deps.py` fails loudly when one
moves. Floating a dependency underneath the crypto is the last place to accept
drift.

**3. It would pull a second microStore.** Upstream declares
`attermann/microStore`; we pin `wet-bulb/microStore`. One build, two sources for
the same library.

Not a reason: `attermann/Crypto` was **already** pinned in our build, so the
AES-256-CTR & HMAC primitives added no new dependency surface.

## Local changes

Kept to the minimum, each marked in place with `THICKET:`.

1. **`#include <Arduino.h>` dropped from the header.** The library targets
   Arduino only; we also build this on the host for the conformance scenario,
   where that header doesn't exist. Nothing in the file used it, the Arduino
   types it would supply aren't referenced.

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
