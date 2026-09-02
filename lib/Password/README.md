# Password

PBKDF2-derived encryption at rest for a file that has to be opened before the
identity exists. Not written here.

## Origin

| | |
|---|---|
| Upstream | [konsumer/arduino-rns-password](https://github.com/konsumer/arduino-rns-password) |
| Commit | `f01728629034c9c06d3767be6d25729ee3c5cb47` |
| Authored | 2026-08-02 by David Konsumer |
| Vendored | 2026-09-02 |
| License | MIT, full text in `./LICENSE`, copied from the upstream repo root |
| Copyright | © 2026 David Konsumer |

The same code is also proposed upstream as
[attermann/microReticulum#44](https://github.com/attermann/microReticulum/pull/44)
(`lib/password/`), open since 2026-04-07 and unmerged. We take the standalone
repository rather than the PR, so nothing here waits on that review. That PR
covers both halves, the identity vault and the message store; `lib/EncryptedStore`
is the other half.

> **The upstream licence is stated twice and the two statements disagree. This
> project treats it as MIT.**
>
> The repository's `LICENSE` file is MIT and GitHub's API reports `MIT` (checked
> with `gh api repos/konsumer/arduino-rns-password`, 2026-09-02). The same
> commit's `library.json` carries `"license": "Apache-2.0"`.
>
> MIT is the deliberate statement. The commit that added the `LICENSE` file is
> titled `MIT`, dated 2026-08-02, and `library.json` still reflects the file's
> origin inside the Apache-2.0 microReticulum tree where it was first proposed.
> `lib/EncryptedStore` carries the identical discrepancy from the same author on
> the same date, resolved the same way.
>
> Nothing turns on it either way: both are inbound-compatible with our
> GPL-3.0-or-later.

## What it does

`password_protect()` derives two 32-byte keys from a password with
PBKDF2-HMAC-SHA256 at 100,000 iterations over a random 16-byte salt. The first
encrypts with AES-256-CTR, the second authenticates with HMAC-SHA256 over
version, salt, IV and ciphertext. `password_open()` verifies that HMAC before
writing any plaintext, so a wrong password and a corrupted file fail the same
way and neither yields a plaintext oracle.

File layout is `version(1) + salt(16) + IV(16) + ciphertext(N) + HMAC(32)`, 65
bytes of overhead.

## What a short code buys, in seconds

A 6-digit code is 1,000,000 possibilities, about 19.9 bits. Each guess costs
100,000 PBKDF2 iterations, and each iteration is two SHA-256 compressions, so
exhausting the space is 2.0e11 compressions. One high-end GPU runs SHA-256 at
roughly 22 GH/s, which puts a full sweep at **about 9 seconds**.

So a 6-digit code does not protect an identity against anyone who images the
flash. It protects against someone who picks the device up. Reaching a day of
GPU time at this iteration count would need 950,400,000 iterations, which an
nRF52840 cannot run, so the length of the secret is the only lever that moves
this. The placeholder is 6 digits because the thumbwheel is the intended input
and its design isn't settled.

## Local changes

Two, both marked `THICKET:` in place, both to let the host tests build:

- `password.h` drops `<Arduino.h>` and `<RNG.h>`.
- `password.cpp` takes its include paths from the installed `microReticulum/`
  prefix, and draws salt and IV from `RNS::Cryptography::random()` rather than
  the Arduino Crypto `RNG` singleton.
