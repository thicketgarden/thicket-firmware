// Thicket interop scenario: IDENTITY VAULT.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The question this answers
// -------------------------
// The identity is the device. Sealing it under a code is only worth doing if
// opening it returns the SAME identity, and if a wrong code returns nothing at
// all rather than garbage that looks like a key. Both halves are asserted here.
//
// The negative cases matter more than the round trip. A round trip proves the
// writer and reader agree; it cannot prove that a wrong code fails, and a
// vault that opens for any code is worse than no vault, because the boot log
// would then claim an identity was restored.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Utilities/OS.h>
#include <password.h>

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
	printf("%s %s\n", cond ? "[ ok ]" : "[FAIL]", what);
	if (!cond) ++failures;
}

const char* PATH = "vault_test";
const char* CODE = "123456";
const size_t PRV = 64;

}   // namespace

int main() {
	printf("[cpp] vault_vectors starting\n");
	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	// Seal a real identity, not arbitrary bytes: the thing that has to survive
	// is the keypair the device announces with.
	RNS::Identity original;
	const RNS::Bytes prv = original.get_private_key();
	const std::string want_hash = original.hash().toHex();
	check(prv.size() == PRV, "identity private key is 64 bytes");
	check(password_protect(PATH, CODE, prv.data(), prv.size()), "seal writes a vault");

	RNS::Bytes sealed_body;
	const size_t sealed = RNS::Utilities::OS::read_file(PATH, sealed_body);
	printf("[info] vault is %zu bytes for a %zu byte key (%zu overhead)\n",
	       sealed, PRV, sealed - PRV);
	check(sealed == PRV + PASSWORD_FILE_OVERHEAD, "vault size is key + 65 bytes");

	// 1. the right code returns the same identity
	{
		uint8_t out[PRV];
		check(password_open(PATH, CODE, out, sizeof(out)), "correct code opens the vault");
		RNS::Identity restored(false);
		check(restored.load_private_key(RNS::Bytes(out, sizeof(out))),
		      "recovered bytes load as a private key");
		check(restored.hash().toHex() == want_hash,
		      "restored identity has the SAME hash as the original");
	}

	// 2. a wrong code returns nothing. Every digit of a 6-digit code is tried
	//    one position at a time rather than trusting a single wrong guess.
	{
		int opened = 0;
		char wrong[8];
		for (int pos = 0; pos < 6; ++pos) {
			for (char d = '0'; d <= '9'; ++d) {
				strcpy(wrong, CODE);
				if (wrong[pos] == d) continue;
				wrong[pos] = d;
				uint8_t out[PRV];
				if (password_open(PATH, wrong, out, sizeof(out))) ++opened;
			}
		}
		check(opened == 0, "all 54 single-digit-wrong codes are refused");
	}

	// 3. a flipped ciphertext byte fails, and fails the same way
	{
		RNS::Bytes body;
		RNS::Utilities::OS::read_file(PATH, body);
		const size_t ct = 1 + 16 + 16;          // version + salt + IV
		RNS::Bytes tampered;
		uint8_t* w = tampered.writable(body.size());
		memcpy(w, body.data(), body.size());
		w[ct] ^= 0x01;
		RNS::Utilities::OS::write_file("vault_tampered", tampered);
		uint8_t out[PRV];
		check(!password_open("vault_tampered", CODE, out, sizeof(out)),
		      "a single flipped ciphertext bit is refused");
	}

	// 4. a truncated vault fails rather than reading past the end
	{
		RNS::Bytes body;
		RNS::Utilities::OS::read_file(PATH, body);
		RNS::Utilities::OS::write_file("vault_short", body.left(body.size() - 1));
		uint8_t out[PRV];
		check(!password_open("vault_short", CODE, out, sizeof(out)),
		      "a truncated vault is refused");
	}


	// 5. attempt limiting, modelled on the firmware's own counter. The point
	//    of the test is the fail-closed order: the counter is persisted before
	//    the code is checked, so pulling power mid-attempt cannot buy a free
	//    guess. A counter written only after a failure would give an attacker
	//    unlimited tries by resetting the board each time.
	{
		const char* TRIES = "vault_tries";
		const uint8_t LIMIT = 10;
		auto tries_read = [&]() -> uint8_t {
			if (!RNS::Utilities::OS::file_exists(TRIES)) return 0;
			RNS::Bytes b;
			if (RNS::Utilities::OS::read_file(TRIES, b) < 1) return 0;
			return b.data()[0];
		};
		auto attempt = [&](const char* code) -> bool {
			const uint8_t tried = tries_read();
			if (tried >= LIMIT) return false;                 // locked out
			uint8_t next = (uint8_t)(tried + 1);
			RNS::Utilities::OS::write_file(TRIES, RNS::Bytes(&next, 1));
			uint8_t out[PRV];
			if (!password_open(PATH, code, out, sizeof(out))) return false;
			RNS::Utilities::OS::remove_file(TRIES);           // cleared on success
			return true;
		};

		RNS::Utilities::OS::remove_file(TRIES);
		for (int i = 0; i < 9; ++i) attempt("000000");
		check(tries_read() == 9, "nine wrong codes leave the counter at nine");
		check(attempt(CODE), "the correct code still opens on the tenth attempt");
		check(tries_read() == 0, "a success clears the counter");

		// now exhaust it
		RNS::Utilities::OS::remove_file(TRIES);
		for (int i = 0; i < LIMIT; ++i) attempt("000000");
		check(tries_read() == LIMIT, "ten wrong codes reach the limit");
		check(!attempt(CODE), "the CORRECT code is refused once locked out");
	}

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
