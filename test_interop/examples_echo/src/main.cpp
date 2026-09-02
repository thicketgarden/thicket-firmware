// Thicket interop scenario: THE REFERENCE'S OWN EXAMPLE.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every other scenario here talks to a Python script we wrote. That proves we
// agree with our own reading of the protocol. This one talks to
// `Examples/Echo.py` out of the Reticulum source tree, unmodified, at the
// version we pin.
//
// The Reticulum community's rules for LLM-assisted projects name this exact
// bar: a port that cannot work with a script from the Examples folder is not
// compatible, whatever its README claims.
//
// Shape: Echo.py runs as the server. It announces
// `example_utilities.echo.request` & sets PROVE_ALL on it. We are the client:
// we learn the destination from its announce, send a payload, & require a
// validated delivery proof.
//
// THE PROOF *IS* THE ECHO. Echo.py's server_callback only logs; it sends no
// data packet back, because the destination proves every packet automatically.
// Its own client measures round-trip time from `receipt.status == DELIVERED`
// & nothing else. An earlier version of this scenario waited for a data reply
// that the reference never sends, and failed against correct behaviour.
//
// What the proof establishes is not weak. The destination is SINGLE, so our
// packet was encrypted to Echo.py's identity: it had to derive the shared key,
// decrypt the payload, & return a signed proof that our stack then validated
// against the identity we learned from its announce. Both directions of the
// crypto agreed with the reference implementation.
//
// Nothing about the destination is compiled in. The app name & aspect below
// are Echo.py's own, & the hash is learned over the wire.
//
// STATUS: INCOMPLETE. This builds & both ends come up, but no announce
// crosses. Verified so far: Echo.py binds 127.0.0.1:14286 & announces every
// 5 s; this client binds 127.0.0.1:14287; the aspects match Echo.py's own
// ("example_utilities" + "echo" + "request"); the Python config uses
// `interface_enabled`, which the working scenarios require. What has not been
// checked is whether RNS's UDPInterface & our UDPInterface2 agree on framing,
// which is the remaining suspect. Not wired into run_all.sh until it passes.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Packet.h>
#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>

#include "UDPInterface2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace RNS;

namespace {

// Echo.py's own identifiers. Changing either makes the announce unmatchable.
const char* APP_NAME = "example_utilities";
const char* ASPECT   = "echo.request";

// What we send. The content is not echoed back, so its only job is to be a
// realistic payload for Echo.py to decrypt.
const char* PAYLOAD = "thicket echo probe";

// File scope, matching the working scenarios. A local Interface goes out of
// scope with main's frame while Transport still holds a reference to it, and
// the interface never comes up: no bind, no announce heard, no error.
Interface iface({Type::NONE});

Bytes peer_dest_hash;
bool  peer_known    = false;

class Hearer : public AnnounceHandler {
public:
	Hearer() : AnnounceHandler(getenv("THICKET_SELF_TEST_BREAK")
	                           ? "example_utilities.echo.wrong"
	                           : "example_utilities.echo.request") {}
	void received_announce(const Bytes& destination_hash,
	                       const Identity& announced_identity,
	                       const Bytes& app_data) {
		(void)announced_identity; (void)app_data;
		if (peer_known) return;
		// The DESTINATION hash, not the identity's: Identity::recall() takes
		// the former, and passing the latter returns nothing while the client
		// waits forever for a peer it already heard.
		peer_dest_hash = destination_hash;
		peer_known = true;
		printf("[echo-client] heard Echo.py at %s\n",
		       destination_hash.toHex().c_str());
		fflush(stdout);
	}
};
HAnnounceHandler hearer(new Hearer());

}   // namespace

int main() {
	// Prove the assertions can fail. With the aspect mangled the announce no
	// longer matches, nothing is learned, and the run must FAIL. A scenario
	// that has only ever passed is an assumption.
	const bool self_test_break = getenv("THICKET_SELF_TEST_BREAK") != nullptr;
	if (self_test_break) {
		printf("[echo-client] --self-test-break: listening on the wrong aspect\n");
		fflush(stdout);
	}

	const char* t = getenv("THICKET_INTEROP_TIMEOUT_S");
	const double timeout_s = t ? atof(t) : 90.0;

	static microStore::Adapters::UniversalFileSystem filesystem;
	filesystem.init();
	Utilities::OS::register_filesystem(filesystem);

	iface = new UDPInterface2("echo", 14287, 14286);
	iface.mode(Type::Interface::MODE_GATEWAY);
	Transport::register_interface(iface);
	iface.start();

	Reticulum reticulum;
	reticulum.transport_enabled(false);
	reticulum.start();

	Identity identity;
	Transport::register_announce_handler(hearer);

	printf("[echo-client] waiting for an announce from Echo.py "
	       "(%s.%s), timeout %.0fs\n", APP_NAME, ASPECT, timeout_s);
	fflush(stdout);

	const double t0 = Utilities::OS::time();
	bool sent = false;
	bool proved = false;
	Packet outbound({Type::NONE});

	while (Utilities::OS::time() - t0 < timeout_s) {
		Transport::loop();
		// Reticulum::loop() is what walks the registered interfaces and calls
		// each one's loop(), which is where the UDP socket is actually read.
		// Transport::loop() alone leaves datagrams sitting unread in the kernel
		// buffer: the peer announces, the bytes arrive, and nothing consumes them.
		reticulum.loop();

		if (peer_known && !sent) {
			Identity peer = Identity::recall(peer_dest_hash);
			if (!peer) continue;
			Destination to(peer, Type::Destination::OUT,
			               Type::Destination::SINGLE, APP_NAME, ASPECT);
			outbound = Packet(to, Bytes((const uint8_t*)PAYLOAD, strlen(PAYLOAD)));
			outbound.send();
			sent = true;
			printf("[echo-client] sent %zu bytes to the echo destination\n",
			       strlen(PAYLOAD));
			fflush(stdout);
		}

		if (sent && !proved) {
			PacketReceipt r = outbound.receipt();
			if (r && r.status() == Type::PacketReceipt::DELIVERED) {
				proved = true;
				printf("[echo-client] delivery proof validated\n");
				fflush(stdout);
			}
		}

		if (proved) break;
		Utilities::OS::sleep(0.05);
	}

	printf("\n[echo-client] === result ===\n");
	printf("[echo-client] heard the announce : %s\n", peer_known ? "yes" : "NO");
	printf("[echo-client] sent a packet      : %s\n", sent ? "yes" : "NO");
	printf("[echo-client] proof validated    : %s\n", proved ? "yes" : "NO");

	if (peer_known && sent && proved) {
		printf("[echo-client] SUCCESS: interoperated with the reference's own "
		       "example, unmodified\n");
		return 0;
	}
	printf("[echo-client] FAIL\n");
	return 1;
}
