#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 7, Python half: READ WHAT THE C++ SIDE WROTE.
#
# The C++ half proves we can decrypt a file upstream's device produced. That is
# the stronger direction, but it is only one direction. It says nothing about
# our WRITE path, because we did not write that file -- and our own reader would
# happily accept our own writer's output even if the two agreed on something
# wrong. A round trip cannot detect a shared mistake; that is the whole reason
# this scenario exists rather than a unit test.
#
# So this half re-implements the on-disk format from the specification, in a
# different language, on a different crypto library, and opens the file the C++
# side just wrote. Nothing is shared between the two implementations except the
# format itself.
#
# The format (upstream's `encrypted_store.h`):
#
#   file    = version(1) || IV(16) || ciphertext(N) || HMAC-SHA256(32)
#   keys    = HKDF-SHA256(ikm=x25519_private_key, salt=identity_hash,
#                         info=b'', length=64)
#   enc_key = keys[0:32]   -> AES-256-CTR
#   mac_key = keys[32:64]  -> HMAC-SHA256 over version || IV || ciphertext
#
# Exit 0 iff the file decrypts to exactly the expected bytes.

import argparse
import os
import sys
import time

from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

FILE_OVERHEAD = 49  # version(1) + IV(16) + HMAC(32)

# The identity from upstream's identity_vault example, unlocked with the
# password its README states. Same key the C++ half loads -- see
# encstore_vectors/src/vectors.h for the provenance.
PRIVATE_KEY = bytes.fromhex(
    "00a78c8d2037a0c1d022f58b4ae6c9c2c5eea5404d9289083881103c1f608351"
    "2b8ea9580e68fa993ff3e6e60c0769df4787c855dd929962c49cf99d913b502b"
)

# Must stay byte-identical to xplain[] in encstore_vectors/src/main.cpp.
# The NUL and the 0xFF are deliberate: a length bug survives ASCII payloads.
XCHECK_PLAINTEXT = (
    b"thicket wrote this"
    b"\x00\xff\xe2\x80\x94\x20ok"
)

checks = 0
failures = 0


def check(ok, what, detail):
    global checks, failures
    checks += 1
    if ok:
        print(f"[python]   OK   {what:<24} {detail}", flush=True)
    else:
        failures += 1
        print(f"[python]   FAIL {what:<24} {detail}", flush=True)


def identity_hash(prv: bytes) -> bytes:
    """Reticulum's identity hash: SHA-256 of both public keys, truncated to 16.

    Derived here rather than passed in, so that a disagreement about the salt
    shows up as a decryption failure in this script too, not only in C++.
    """
    x_pub = X25519PrivateKey.from_private_bytes(prv[0:32]).public_key()
    ed_pub = Ed25519PrivateKey.from_private_bytes(prv[32:64]).public_key()

    from cryptography.hazmat.primitives import serialization
    raw = serialization.Encoding.Raw
    pub_fmt = serialization.PublicFormat.Raw

    digest = hashes.Hash(hashes.SHA256())
    digest.update(x_pub.public_bytes(raw, pub_fmt))
    digest.update(ed_pub.public_bytes(raw, pub_fmt))
    return digest.finalize()[:16]


def derive_keys(prv: bytes) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(),
        length=64,
        salt=identity_hash(prv),
        info=b"",
    ).derive(prv[0:32])


def open_message(path: str, prv: bytes) -> bytes:
    raw = open(path, "rb").read()
    if len(raw) < FILE_OVERHEAD:
        raise ValueError(f"file is {len(raw)} bytes, below the {FILE_OVERHEAD}-byte overhead")

    version = raw[0:1]
    iv = raw[1:17]
    ct = raw[17:-32]
    mac_stored = raw[-32:]

    keys = derive_keys(prv)
    enc_key, mac_key = keys[0:32], keys[32:64]

    # Authenticate before decrypting, the same ordering the C++ side uses. A
    # verify() mismatch raises.
    h = hmac.HMAC(mac_key, hashes.SHA256())
    h.update(version)
    h.update(iv)
    h.update(ct)
    h.verify(mac_stored)

    dec = Cipher(algorithms.AES(enc_key), modes.CTR(iv)).decryptor()
    return dec.update(ct) + dec.finalize()


def wait_for(path: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        # Wait for the size to settle as well as the file to exist, so a
        # partially-flushed write is not read as corruption.
        if os.path.exists(path) and os.path.getsize(path) > FILE_OVERHEAD:
            return True
        time.sleep(0.1)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True,
                    help="directory the C++ side writes ours.enc into")
    ap.add_argument("--timeout", type=float, default=40.0)
    ap.add_argument("--self-test-break", choices=("none", "tamper"),
                    default="none")
    args = ap.parse_args()

    path = os.path.join(args.dir, "ours.enc")
    print(f"[python] waiting for the C++ side to write {path}", flush=True)

    if not wait_for(path, args.timeout):
        print(f"[python] FAILURE no file at {path} after {args.timeout}s",
              flush=True)
        sys.exit(2)

    size = os.path.getsize(path)
    print(f"[python] found it: {size} bytes "
          f"({size - FILE_OVERHEAD} plaintext + {FILE_OVERHEAD} overhead)",
          flush=True)

    if args.self_test_break == "tamper":
        # Prove this script can fail: flip one bit of the ciphertext body.
        raw = bytearray(open(path, "rb").read())
        raw[20] ^= 0x01
        open(path, "wb").write(bytes(raw))
        print("[python] SELF-TEST BREAK: flipped one ciphertext bit; the MAC "
              "must now reject the file", flush=True)

    try:
        plaintext = open_message(path, PRIVATE_KEY)
    except Exception as exc:
        check(False, "decrypt C++ output",
              f"{type(exc).__name__}: {exc}")
        print(f"[python] FAILURE {failures} of {checks} checks failed",
              flush=True)
        sys.exit(1)

    check(True, "decrypt C++ output",
          "authenticated and decrypted a file the C++ side wrote")
    check(plaintext == XCHECK_PLAINTEXT, "plaintext matches",
          f"{len(plaintext)} bytes, NUL and 0xFF intact"
          if plaintext == XCHECK_PLAINTEXT
          else f"got {plaintext!r}, want {XCHECK_PLAINTEXT!r}")

    if failures:
        print(f"[python] FAILURE {failures} of {checks} checks failed",
              flush=True)
        sys.exit(1)

    print(f"[python] SUCCESS all {checks} checks passed; an independent "
          f"implementation read what our C++ side wrote", flush=True)


if __name__ == "__main__":
    main()
