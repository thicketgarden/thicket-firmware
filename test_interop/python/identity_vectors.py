#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Reference vectors for RNS Identity, generated from and verified against the
Python RNS reference.

Why this scenario exists (T28 priority 3): every interop scenario uses Identity
and none asserts it. A scenario can pass while key derivation is subtly wrong,
as long as both sides are wrong in the same way -- which is exactly what
happens when the C++ side is only ever tested against itself.

Two modes:

  --emit     print the C++ header of vectors to stdout. Run this to regenerate
             identity_vectors/src/vectors.h when the RNS pin moves (A28').

  --verify   the Python half of the scenario. Re-derives every deterministic
             vector from the committed private key and compares it to the
             committed header, then independently checks -- under RNS, not
             under our own arithmetic -- that the committed signature verifies
             and the committed ciphertext decrypts.

The split matters. The deterministic vectors can be regenerated and diffed, so
a moved pin shows up immediately. The ciphertext cannot: RNS's Identity.encrypt
draws an ephemeral X25519 key from the `cryptography` backend's RNG, which is
not seedable from here, so a regenerated ciphertext would differ every run for
reasons that mean nothing. It is therefore pinned once and checked for validity
rather than for equality -- which is the stronger check anyway.
"""

import argparse
import hashlib
import re
import sys

try:
    import RNS
    from RNS.Cryptography import HKDF
except ImportError as e:                                  # pragma: no cover
    print(f"[python] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

# ---------------------------------------------------------------------------
# Fixed inputs. Derived from labelled strings rather than typed at random, so
# anyone can re-derive them and see there is nothing up our sleeve.
# ---------------------------------------------------------------------------
PRIVATE_KEY = (hashlib.sha256(b"thicket-interop-identity-vector/x25519").digest()
               + hashlib.sha256(b"thicket-interop-identity-vector/ed25519").digest())
MESSAGE = b"thicket interop identity vector message"
PLAINTEXT = b"thicket interop identity vector plaintext"
APP_NAME = "thicket_interop"
ASPECTS = "vectors"
HKDF_SECRET = b"thicket-interop-hkdf-secret"
HKDF_SALT = b"thicket-interop-hkdf-salt"
HKDF_CONTEXT = b"thicket-interop-hkdf-context"
HKDF_LENGTH = 64

HEADER_RE = re.compile(r'VEC_([A-Z0-9_]+)\s*=\s*"([0-9a-f]*)"')


def reference_identity():
    identity = RNS.Identity(create_keys=False)
    identity.load_private_key(PRIVATE_KEY)
    return identity


def deterministic_vectors():
    """Everything that can be re-derived from PRIVATE_KEY alone."""
    identity = reference_identity()
    return {
        "PRIVATE_KEY": PRIVATE_KEY.hex(),
        "PUBLIC_KEY": identity.get_public_key().hex(),
        "IDENTITY_HASH": identity.hash.hex(),
        "DEST_HASH": RNS.Destination.hash(identity, APP_NAME, ASPECTS).hex(),
        "MESSAGE": MESSAGE.hex(),
        "FULL_HASH": RNS.Identity.full_hash(MESSAGE).hex(),
        "TRUNCATED_HASH": RNS.Identity.truncated_hash(MESSAGE).hex(),
        "SIGNATURE": identity.sign(MESSAGE).hex(),
        "PLAINTEXT": PLAINTEXT.hex(),
        "HKDF_SECRET": HKDF_SECRET.hex(),
        "HKDF_SALT": HKDF_SALT.hex(),
        "HKDF_CONTEXT": HKDF_CONTEXT.hex(),
        # Three HKDF cases. The empty-context one is the shape every encrypted
        # packet actually uses (Identity.get_context() returns nothing on both
        # sides); the other two exist to find out whether the parameters that
        # are never exercised in production behave like the reference. One of
        # them does not -- see identity_vectors/src/main.cpp.
        "HKDF_OUTPUT": HKDF.hkdf(length=HKDF_LENGTH, derive_from=HKDF_SECRET,
                                 salt=HKDF_SALT, context=HKDF_CONTEXT).hex(),
        "HKDF_OUTPUT_NOCTX": HKDF.hkdf(length=HKDF_LENGTH,
                                       derive_from=HKDF_SECRET,
                                       salt=HKDF_SALT, context=None).hex(),
        "HKDF_OUTPUT_NOSALT": HKDF.hkdf(length=HKDF_LENGTH,
                                        derive_from=HKDF_SECRET,
                                        salt=None, context=None).hex(),
    }


def emit(out):
    identity = reference_identity()
    vectors = deterministic_vectors()
    # Pinned, not regenerated: see the module docstring.
    vectors["CIPHERTEXT"] = identity.encrypt(PLAINTEXT).hex()

    out.write(f"""// GENERATED FILE -- do not hand-edit.
//
// Reference vectors for RNS Identity, produced by the Python reference.
// Regenerate with:
//
//   PATH=/tmp/rnsvenv/bin:$PATH python3 test_interop/python/identity_vectors.py \\
//       --emit > test_interop/identity_vectors/src/vectors.h
//
// Generated against RNS {RNS.__version__}.
//
// Every value below except VEC_CIPHERTEXT is a pure function of
// VEC_PRIVATE_KEY and the fixed inputs, and is re-derived and diffed on every
// run by identity_vectors.py --verify. VEC_CIPHERTEXT is pinned because RNS
// draws a fresh ephemeral X25519 key on every encrypt; --verify checks that it
// still decrypts rather than that it still matches.

#pragma once

static const char* VEC_RNS_VERSION = "{RNS.__version__}";
""")
    for name in ("PRIVATE_KEY", "PUBLIC_KEY", "IDENTITY_HASH",
                 "DEST_HASH", "MESSAGE", "FULL_HASH", "TRUNCATED_HASH",
                 "SIGNATURE", "PLAINTEXT", "CIPHERTEXT", "HKDF_SECRET",
                 "HKDF_SALT", "HKDF_CONTEXT", "HKDF_OUTPUT",
                 "HKDF_OUTPUT_NOCTX", "HKDF_OUTPUT_NOSALT"):
        out.write(f'static const char* VEC_{name} = "{vectors[name]}";\n')
    out.write(f'\nstatic const char* VEC_APP_NAME = "{APP_NAME}";\n')
    out.write(f'static const char* VEC_ASPECTS  = "{ASPECTS}";\n')
    out.write(f'static const size_t VEC_HKDF_LENGTH = {HKDF_LENGTH};\n')


def parse_header(path):
    with open(path) as fh:
        text = fh.read()
    return dict(HEADER_RE.findall(text))


def verify(header_path, break_mode="none"):
    committed = parse_header(header_path)
    if not committed:
        print(f"[python] FAIL: no vectors parsed out of {header_path}",
              flush=True)
        return 1

    failures = 0
    checks = 0

    def check(name, got, want):
        nonlocal failures, checks
        checks += 1
        if got == want:
            print(f"[python]   OK   {name}", flush=True)
        else:
            failures += 1
            print(f"[python]   FAIL {name}", flush=True)
            print(f"[python]          committed: {want}", flush=True)
            print(f"[python]          reference: {got}", flush=True)

    print(f"[python] re-deriving vectors under RNS {RNS.__version__}",
          flush=True)
    for name, value in deterministic_vectors().items():
        if name not in committed:
            failures += 1
            checks += 1
            print(f"[python]   FAIL {name} missing from the committed header",
                  flush=True)
            continue
        check(name, value, committed[name])

    identity = reference_identity()

    # The committed signature must verify under RNS, not merely match a string
    # we also produced.
    checks += 1
    sig = bytes.fromhex(committed.get("SIGNATURE", ""))
    if identity.validate(sig, MESSAGE):
        print("[python]   OK   SIGNATURE verifies under RNS", flush=True)
    else:
        failures += 1
        print("[python]   FAIL committed SIGNATURE does not verify under RNS",
              flush=True)

    # And a corrupted one must not, or "verifies" means nothing.
    checks += 1
    bad = bytearray(sig)
    bad[-1] ^= 0x01
    if identity.validate(bytes(bad), MESSAGE):
        failures += 1
        print("[python]   FAIL RNS accepted a corrupted signature", flush=True)
    else:
        print("[python]   OK   corrupted SIGNATURE rejected by RNS", flush=True)

    # The pinned ciphertext must still decrypt to the pinned plaintext.
    checks += 1
    ct = bytes.fromhex(committed.get("CIPHERTEXT", ""))
    if break_mode == "ciphertext":
        ct = ct[:-1] + bytes([ct[-1] ^ 0x01])
        print("[python] SELF-TEST BREAK: flipped one bit of the committed "
              "ciphertext", flush=True)
    try:
        recovered = identity.decrypt(ct)
    except Exception as e:
        recovered = None
        print(f"[python]   (decrypt raised {e})", flush=True)
    if recovered == PLAINTEXT:
        print("[python]   OK   CIPHERTEXT decrypts under RNS to PLAINTEXT",
              flush=True)
    else:
        failures += 1
        print("[python]   FAIL committed CIPHERTEXT did not decrypt to "
              f"PLAINTEXT (got {recovered!r})", flush=True)

    if failures:
        print(f"[python] FAILURE {failures} of {checks} vector checks failed",
              flush=True)
        return 1
    print(f"[python] SUCCESS all {checks} vector checks passed against "
          f"RNS {RNS.__version__}", flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true",
                    help="print the C++ vectors header to stdout")
    ap.add_argument("--verify", metavar="HEADER",
                    help="re-derive and check a committed vectors header")
    # Accepted and ignored: the shared driver passes it to every Python side.
    ap.add_argument("--timeout", type=float, default=40.0)
    ap.add_argument("--self-test-break", choices=("none", "ciphertext"),
                    default="none",
                    help="deliberately break one assertion, to prove it is live")
    args = ap.parse_args()

    if args.emit:
        emit(sys.stdout)
        return 0
    if args.verify:
        return verify(args.verify, args.self_test_break)
    ap.error("one of --emit or --verify is required")


if __name__ == "__main__":
    sys.exit(main())
