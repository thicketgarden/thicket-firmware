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
// `example_utilities.echo.request` & echoes any packet back, proving each one.
// We are the client. We learn the destination from its announce, send a
// payload, & require both the echo and a validated delivery proof.
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

// What we send. Echo.py returns the packet's data verbatim, so the reply must
// match byte for byte.
const char* PAYLOAD = "thicket echo probe";

// File scope, matching the working scenarios. A local Interface goes out of
// scope with main's frame while Transport still holds a reference to it, and
// the interface never comes up: no bind, no announce heard, no error.
Interface iface({Type::NONE});

Bytes peer_dest_hash;
bool  peer_known    = false;
bool  echo_received = false;

class Hearer : public AnnounceHandler {
public:
	Hearer() : AnnounceHandler("example_utilities.echo.request") {}
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

void on_reply(const Bytes& data, const Packet& packet) {
	(void)packet;
	const std::string got((const char*)data.data(), data.size());
	if (got == PAYLOAD) {
		echo_received = true;
		printf("[echo-client] echo matched: \"%s\"\n", got.c_str());
	} else {
		printf("[echo-client] echo MISMATCH: sent \"%s\", got \"%s\"\n",
		       PAYLOAD, got.c_str());
	}
	fflush(stdout);
}

}   // namespace

int main() {
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

		if (peer_known && !sent) {
			Identity peer = Identity::recall(peer_dest_hash);
			if (!peer) continue;
			Destination to(peer, Type::Destination::OUT,
			               Type::Destination::SINGLE, APP_NAME, ASPECT);
			to.set_packet_callback(on_reply);
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

		if (proved && echo_received) break;
		Utilities::OS::sleep(0.05);
	}

	printf("\n[echo-client] === result ===\n");
	printf("[echo-client] heard the announce : %s\n", peer_known ? "yes" : "NO");
	printf("[echo-client] sent a packet      : %s\n", sent ? "yes" : "NO");
	printf("[echo-client] proof validated    : %s\n", proved ? "yes" : "NO");
	printf("[echo-client] echo matched       : %s\n", echo_received ? "yes" : "NO");

	if (peer_known && sent && proved && echo_received) {
		printf("[echo-client] PASS: interoperated with the reference's own "
		       "example, unmodified\n");
		return 0;
	}
	printf("[echo-client] FAIL\n");
	return 1;
}
