// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference vectors for encrypted-at-rest message storage.
//
// PROVENANCE. None of this was produced by us. The ciphertext is the file
// `examples/identity_vault/test/msgs/last.enc` shipped with
// konsumer/arduino-rns-encrypted-store (also attermann/microReticulum#44),
// written by the author's own device, and the identity is the one it was
// written for -- `test/identity.bin` from the same example, unlocked with the
// password the example's README states (`1234`) using the author's own
// `identity_tool.py`.
//
// That is the whole point of this file. Round-tripping our own output proves
// only that we agree with ourselves. Decrypting a ciphertext someone else
// produced, with an implementation that shares no code with theirs above the
// primitives, is what proves the on-disk format.
//
// [V, read at source 2026-08-05: repo cloned at 5ccdc65, vector decrypted with
//     upstream's identity_tool.py, plaintext reproduced below verbatim]

#pragma once
#include <stdint.h>
#include <stddef.h>

// The identity's 64-byte private key: X25519(32) || Ed25519(32).
// Identity::load_private_key() splits it exactly there.
static const char* VEC_PRIVATE_KEY_HEX =
	"00a78c8d2037a0c1d022f58b4ae6c9c2c5eea5404d9289083881103c1f608351"
	"2b8ea9580e68fa993ff3e6e60c0769df4787c855dd929962c49cf99d913b502b";

// What upstream's tool reports for that key. Checked first, because if our
// Identity disagrees here then every later check is testing the wrong key.
static const char* VEC_IDENTITY_HASH_HEX = "1e550f0e4dab334c704b03e14e579fdb";

// The plaintext upstream's tool recovers from the ciphertext below.
static const char* VEC_PLAINTEXT = "The quick brown fox jumps over the lazy dog";
static const size_t VEC_PLAINTEXT_LEN = 43;

// last.enc, byte for byte: version(1) || IV(16) || ciphertext(43) || HMAC(32).
static const uint8_t VEC_CIPHERTEXT[] = {
	0x01, 0x5d, 0x2f, 0x31, 0x2b, 0x66, 0x94, 0x56, 0x22, 0x97, 0xca, 0x47,
	0x70, 0x23, 0x95, 0x80, 0x8f, 0x49, 0xaa, 0xef, 0x97, 0x05, 0xfc, 0xc5,
	0x1d, 0xe6, 0xa8, 0x66, 0xfe, 0x56, 0x99, 0x4a, 0x04, 0xaa, 0x73, 0x3a,
	0x75, 0x7b, 0x0d, 0xe5, 0xbc, 0x80, 0x44, 0x7b, 0xd4, 0x9c, 0xd7, 0x96,
	0x6f, 0x9b, 0xf8, 0x3f, 0x4a, 0x36, 0x2e, 0x46, 0x97, 0x49, 0x39, 0x5d,
	0x99, 0xc2, 0x6b, 0x46, 0x37, 0x37, 0x02, 0x81, 0x74, 0x9c, 0xd2, 0x7f,
	0x44, 0xca, 0xe0, 0x5f, 0x82, 0x9e, 0x8c, 0xff, 0xa8, 0x7e, 0x29, 0xee,
	0x3b, 0x5f, 0xd4, 0xca, 0x3e, 0xd5, 0x52, 0x68,
};
static const size_t VEC_CIPHERTEXT_LEN = sizeof(VEC_CIPHERTEXT);
