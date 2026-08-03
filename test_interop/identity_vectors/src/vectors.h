// GENERATED FILE -- do not hand-edit.
//
// Reference vectors for RNS Identity, produced by the Python reference.
// Regenerate with:
//
//   PATH=/tmp/rnsvenv/bin:$PATH python3 test_interop/python/identity_vectors.py \
//       --emit > test_interop/identity_vectors/src/vectors.h
//
// Generated against RNS 1.4.2.
//
// Every value below except VEC_CIPHERTEXT is a pure function of
// VEC_PRIVATE_KEY and the fixed inputs, and is re-derived and diffed on every
// run by identity_vectors.py --verify. VEC_CIPHERTEXT is pinned because RNS
// draws a fresh ephemeral X25519 key on every encrypt; --verify checks that it
// still decrypts rather than that it still matches.

#pragma once

static const char* VEC_RNS_VERSION = "1.4.2";
static const char* VEC_PRIVATE_KEY = "59226421674550a79da6a73a9a82565ff5b4e4c362254a1621fb3e1fe8ab46e2f5c2d08784085fe83b5661cc773e2cb2922835d3e27720e20510e0c91f95f87d";
static const char* VEC_PUBLIC_KEY = "c4dc1b6b31ba494af7272b1be56b7fd4844898618c8d43665e6dc78121dd8167c5c4875ba417e3371981629cdfd54dccd1d79860359b2676690d1638cfcff9ac";
static const char* VEC_IDENTITY_HASH = "7ed1422452671d978f312e20efc712f2";
static const char* VEC_DEST_HASH = "8e67bfde320ce4a1bfd29651296bcaff";
static const char* VEC_MESSAGE = "746869636b657420696e7465726f70206964656e7469747920766563746f72206d657373616765";
static const char* VEC_FULL_HASH = "1bdc14d139a5747b1c658e0515e5b3cf707eaecef78ba6688dbc1e2fd332a5ff";
static const char* VEC_TRUNCATED_HASH = "1bdc14d139a5747b1c658e0515e5b3cf";
static const char* VEC_SIGNATURE = "ce0cc2fb61cfcf70248ba3edcad02ca4b010f2e1e583c05f260643d3386dfb3c9be699088197e0dbf413e25f58b33d4a314cbc082b1d5b67fbb641dc8da49d0d";
static const char* VEC_PLAINTEXT = "746869636b657420696e7465726f70206964656e7469747920766563746f7220706c61696e74657874";
static const char* VEC_CIPHERTEXT = "1d29bde090702d82775f4bd071a4fa49ec5505e8ac6863a309e13ddbf07e1a0ae379b165aea476ed84af9ab1ca5bee3a2a7d2c74d8b4c1b49c9fd462549bbed618f688638971042dbf507970d749d22adb441a62c25286f088db1472fa10c039d64da9fe22045ae6c3996de0fad67cb75bf60894be45ae1e559352be89bb4593";
static const char* VEC_HKDF_SECRET = "746869636b65742d696e7465726f702d686b64662d736563726574";
static const char* VEC_HKDF_SALT = "746869636b65742d696e7465726f702d686b64662d73616c74";
static const char* VEC_HKDF_CONTEXT = "746869636b65742d696e7465726f702d686b64662d636f6e74657874";
static const char* VEC_HKDF_OUTPUT = "30729fed0e993d6d226fb1a776d283cdac861757df852ca86accf5852727dd56992b53d681c3386a6e81844bbbed306aac6400252a292ec301b3eaa954a21a9a";
static const char* VEC_HKDF_OUTPUT_NOCTX = "d2627d8d8436d0cb681f7f7a9187fa3bfbdf5befdf1005596d9b4edb0127dc7d0133f67215a199eb5c32046ace9199ec9f8afd7024604bc3cfb9242aee121739";
static const char* VEC_HKDF_OUTPUT_NOSALT = "7db1c82a75cd5aacb9d9c71ba62158e06f140ff6c3182df8b97098b3c0da76f044670293f1ffd68f517f2e099eb47392abb385d20febea45a34ab7e5a3a1aefd";

static const char* VEC_APP_NAME = "thicket_interop";
static const char* VEC_ASPECTS  = "vectors";
static const size_t VEC_HKDF_LENGTH = 64;
