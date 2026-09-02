// Thicket interop scenario 7: ENCRYPTED-AT-REST MESSAGE STORAGE.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// A store that encrypts and decrypts with itself will round-trip perfectly
// while being wrong in any way both halves share -- the same failure mode the
// identity-vector scenario exists for, and the reason that one asserts against
// reference output instead of its own.
//
// So the load-bearing check here is not our round trip. It's that we can read
// a 92-byte file WE DID NOT WRITE: `test/msgs/last.enc` from upstream's
// identity_vault example, produced by the author's device, decrypted here with
// an implementation sharing no code with theirs above the crypto primitives.
// If the derivation, the salt, the layout or the MAC ordering disagreed by one
// byte, that check fails and nothing else in this file would have noticed.
//
// The negative checks matter as much. Authenticated encryption whose
// authentication never rejects anything is just encryption, and a store that
// silently returns plaintext for the wrong identity is worse than one that
// stores nothing.
//
// Exit 0 iff every check passes.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <microReticulum.h>

#include "encrypted_store.h"
#include "vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int checks = 0;
static int failures = 0;

static void check_bool(const char* what, bool got, bool want, const char* note) {
	++checks;
	if (got == want) {
		printf("[cpp]   OK   %-24s %s\n", what, note);
	}
	else {
		++failures;
		printf("[cpp]   FAIL %-24s got %s, want %s -- %s\n", what,
		       got ? "true" : "false", want ? "true" : "false", note);
	}
}

static void check_str(const char* what, const std::string& got, const char* want) {
	++checks;
	if (got == want) {
		printf("[cpp]   OK   %-24s '%s'\n", what, got.c_str());
	}
	else {
		++failures;
		printf("[cpp]   FAIL %-24s\n", what);
		printf("[cpp]          got:  '%s'\n", got.c_str());
		printf("[cpp]          want: '%s'\n", want);
	}
}

// Write raw bytes straight to a path, bypassing the encrypted layer. Used to
// place upstream's ciphertext on disk exactly as their device wrote it.
static bool put_raw(const char* path, const uint8_t* data, size_t len) {
	RNS::Bytes blob(data, len);
	return RNS::Utilities::OS::write_file(path, blob) == len;
}

int main() {
	printf("[cpp] encstore_vectors starting\n");

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	// --- the identity the reference vector was written for -----------------
	RNS::Bytes prv;
	prv.appendHex(VEC_PRIVATE_KEY_HEX);

	RNS::Identity identity(false);
	if (!identity.load_private_key(prv)) {
		printf("[cpp] FAIL could not load the reference private key\n");
		printf("[cpp] exit code 1\n");
		return 1;
	}

	// Checked before anything else. If our identity hash disagrees with
	// upstream's tool then the HKDF salt disagrees too, and every check below
	// would be exercising a different key while looking like a format bug.
	check_str("identity hash", identity.hash().toHex(), VEC_IDENTITY_HASH_HEX);

	// --- THE CHECK: read a file the author's device wrote ------------------
	const char* vector_path = "last.enc";
	if (!put_raw(vector_path, VEC_CIPHERTEXT, VEC_CIPHERTEXT_LEN)) {
		printf("[cpp] FAIL could not stage the reference ciphertext\n");
		printf("[cpp] exit code 1\n");
		return 1;
	}

	// The file carries no length field: its plaintext size is file size minus
	// the 49 bytes of overhead. Asserted, because a wrong answer here would
	// make the decrypt below fail for a reason that has nothing to do with
	// the crypto.
	const size_t sized = encstore_size(vector_path);
	check_bool("plaintext size", sized == VEC_PLAINTEXT_LEN, true,
	           sized == VEC_PLAINTEXT_LEN ? "43 bytes, from a 92-byte file"
	                                      : "size accounting disagrees");

	std::vector<uint8_t> out(VEC_PLAINTEXT_LEN + 1, 0);
	const bool read_ok = encstore_read(vector_path, identity,
	                                   out.data(), VEC_PLAINTEXT_LEN);
	check_bool("read foreign file", read_ok, true,
	           "authenticated and decrypted a ciphertext we did not produce");
	if (read_ok) {
		check_str("foreign plaintext",
		          std::string((const char*)out.data(), VEC_PLAINTEXT_LEN),
		          VEC_PLAINTEXT);
	}

	// --- our own round trip ------------------------------------------------
	// Weaker evidence than the above, but it covers the write path, which the
	// reference vector can't: upstream produced that file, not us.
	const char* ours_path = "ours.enc";
	const char* payload = "thicket round trip \xE2\x80\x94 non-ascii and 0x00 follow";
	std::vector<uint8_t> plain(payload, payload + strlen(payload));
	plain.push_back(0x00);          // a NUL mid-blob: this isn't a C string
	plain.push_back(0xFF);

	check_bool("write", encstore_write(ours_path, identity,
	                                   plain.data(), plain.size()), true,
	           "encrypted and wrote our own blob");

	std::vector<uint8_t> back(plain.size(), 0);
	const bool ours_ok = encstore_read(ours_path, identity,
	                                   back.data(), plain.size());
	check_bool("round trip", ours_ok && back == plain, true,
	           "byte-identical, NUL and 0xFF included");

	// Two writes of the same plaintext must differ, or the IV isn't random
	// and the whole construction leaks equality of messages.
	RNS::Bytes first, second;
	encstore_write(ours_path, identity, plain.data(), plain.size());
	RNS::Utilities::OS::read_file(ours_path, first);
	encstore_write(ours_path, identity, plain.data(), plain.size());
	RNS::Utilities::OS::read_file(ours_path, second);
	check_bool("fresh IV per write", first != second, true,
	           "same plaintext encrypts to different bytes");

	// --- negative: the authentication has to reject -------------------------
	// Flip one bit of the ciphertext body. CTR mode would happily decrypt it
	// into corrupted plaintext; the HMAC is the only thing standing between
	// that and a caller believing it.
	std::vector<uint8_t> tampered(VEC_CIPHERTEXT,
	                              VEC_CIPHERTEXT + VEC_CIPHERTEXT_LEN);
	tampered[20] ^= 0x01;
	put_raw("tampered.enc", tampered.data(), tampered.size());
	std::vector<uint8_t> scratch(VEC_PLAINTEXT_LEN, 0xAA);
	check_bool("reject tampered", encstore_read("tampered.enc", identity,
	                                            scratch.data(),
	                                            VEC_PLAINTEXT_LEN), false,
	           "one flipped bit in the body is refused");

	// Flipping the version byte must fail too: it's inside the MAC.
	std::vector<uint8_t> reversioned(VEC_CIPHERTEXT,
	                                 VEC_CIPHERTEXT + VEC_CIPHERTEXT_LEN);
	reversioned[0] = 0x02;
	put_raw("reversioned.enc", reversioned.data(), reversioned.size());
	check_bool("reject bad version", encstore_read("reversioned.enc", identity,
	                                               scratch.data(),
	                                               VEC_PLAINTEXT_LEN), false,
	           "the version byte is covered by the MAC");

	// A different identity must not read it. This is the property the whole
	// feature exists for -- and the one that makes destroying the identity a
	// crypto-erase of every stored message.
	RNS::Identity other(true);
	check_bool("reject wrong identity",
	           encstore_read(vector_path, other, scratch.data(),
	                         VEC_PLAINTEXT_LEN), false,
	           "another identity cannot read these messages");

	// Truncation must not be mistaken for a short message.
	put_raw("short.enc", VEC_CIPHERTEXT, VEC_CIPHERTEXT_LEN - 8);
	check_bool("reject truncated", encstore_read("short.enc", identity,
	                                             scratch.data(),
	                                             VEC_PLAINTEXT_LEN), false,
	           "a chopped file is refused, not silently shortened");

	// A missing file is a miss, not a crash and not a stale buffer.
	check_bool("missing file", encstore_read("nope.enc", identity,
	                                         scratch.data(),
	                                         VEC_PLAINTEXT_LEN), false,
	           "absent path returns false");
	check_bool("missing file size", encstore_size("nope.enc") == 0, true,
	           "absent path sizes to 0");

	// --- the other direction: hand a file to the Python half ----------------
	// Everything above proves we can READ upstream's format. Nothing above
	// proves anything else can read what we WRITE -- our reader would accept
	// our writer's output even if both were wrong the same way, which is the
	// exact trap this scenario was built to avoid. So write one more file, in
	// a directory the Python half is told about, and let an implementation
	// that shares no code with ours try to open it.
	//
	// The payload must stay byte-identical to XCHECK_PLAINTEXT in
	// python/encstore_vectors.py. It carries a NUL and a 0xFF on purpose:
	// this is a blob, not a C string, and a length bug would survive ASCII.
	const char* xdir = getenv("THICKET_ENCSTORE_DIR");
	if (xdir != nullptr) {
		static const uint8_t xplain[] = {
			't','h','i','c','k','e','t',' ','w','r','o','t','e',' ','t','h','i','s',
			0x00, 0xFF, 0xE2, 0x80, 0x94, 0x20, 'o','k',
		};
		const std::string xpath = std::string(xdir) + "/ours.enc";
		check_bool("wrote file for python",
		           encstore_write(xpath.c_str(), identity,
		                          xplain, sizeof(xplain)), true,
		           "the Python half decrypts this one");
	}
	else {
		printf("[cpp]   note: THICKET_ENCSTORE_DIR unset, skipping the "
		       "hand-off to the Python half\n");
	}

	int rc = 0;
	if (failures > 0) {
		printf("[cpp] FAIL: %d of %d encrypted-store checks failed\n",
		       failures, checks);
		rc = 1;
	}
	else {
		printf("[cpp] SUCCESS all %d encrypted-store checks passed, including "
		       "decrypting upstream's own %zu-byte vector\n",
		       checks, VEC_CIPHERTEXT_LEN);
	}
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
