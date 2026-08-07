// Thicket interop scenario 8: TWO OF OUR OWN NODES.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WHAT THIS IS FOR, AND WHAT IT IS EXPLICITLY NOT
//
// Every other scenario in this suite has the Python reference ORIGINATE and us
// receive. That is the right primary test and it is the direction a handheld
// spends its life in — but it means our OUTBOUND constructs have only ever been
// validated by whatever the reference happened to accept along the way. Nothing
// makes our stack both produce and consume the same thing.
//
// This does. One of our nodes encrypts, signs and sends; another of our nodes
// receives, decrypts and proves. The failures it can find are the asymmetric
// ones: something we emit that we cannot parse.
//
// ⚠ IT IS NOT EVIDENCE OF CONFORMANCE, and must never be cited as such. Two
// implementations that misread the protocol identically agree with each other
// perfectly. A green run here is strictly weaker than a green reference run,
// because both ends share every assumption. It supplements the suite; it
// replaces nothing.
//
// ONE PROCESS, TWO ROLES
//
// Role comes from THICKET_ROLE, set by the driver. One binary rather than two
// projects so that both ends are provably the same code — if the originator and
// receiver were separate builds, "we can parse what we emit" would be a claim
// about two artefacts rather than one.
//
// TOPOLOGY: a direct UDP pair, not a routed pair.
//
// The task sketched two segments joined by a router. A direct pair proves the
// assertions that matter here — our decode of our encode, and a proof
// validating between two microReticulum instances — with far less machinery,
// and `transport_forwarder` already covers us routing. Routed two-node is a
// worthwhile extension, not a precondition, and it is noted as such rather than
// silently skipped.
//
// Exit 0 iff this role's checks all pass.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <microReticulum.h>
#include <UDPInterface2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// 14280-14283 belong to the forwarding scenario; these are its neighbours so a
// stale process from either cannot quietly join the wrong test.
#define ORIG_LOCAL   14290
#define ORIG_REMOTE  14291
#define RECV_LOCAL   14291
#define RECV_REMOTE  14290

#define APP_NAME     "thicket_interop"
#define ASPECT       "two_node"

static int checks = 0;
static int failures = 0;

static void check(const char* what, bool ok, const char* note) {
	++checks;
	if (ok) {
		printf("[%s]   OK   %-26s %s\n", getenv("THICKET_ROLE"), what, note);
	}
	else {
		++failures;
		printf("[%s]   FAIL %-26s %s\n", getenv("THICKET_ROLE"), what, note);
	}
	fflush(stdout);
}

// The payload deliberately is not round numbers. Per the task: prefer something
// that exercises our own encoders. It carries an embedded NUL, a 0xFF, a
// multi-byte UTF-8 sequence, and a length that is not a multiple of the AES
// block size, so a padding or length bug cannot hide behind a tidy size.
// `originating` decides whether the break applies, because THICKET_BREAK_PAYLOAD
// is exported to BOTH processes by the runner -- corrupting it on both ends
// would make them agree again and the self-test would pass, which is the exact
// failure a self-test exists to rule out.
static RNS::Bytes make_payload(bool corrupt = false) {
	static const uint8_t raw[] = {
		't','w','o','-','n','o','d','e',' ',
		0x00, 0xFF, 0xE2, 0x80, 0x94, ' ',
		'o','u','r',' ','e','n','c','o','d','e',',',' ',
		'o','u','r',' ','d','e','c','o','d','e',
		0x01, 0x7F, 0x80, 0xFE,
	};
	RNS::Bytes out(raw, sizeof(raw));
	if (corrupt) {
		uint8_t* w = out.writable(sizeof(raw));
		w[9] ^= 0x01;   // one bit, inside the body
		printf("[originator] SELF-TEST BREAK: flipped one payload bit; the "
		       "receiver must report a mismatch\n");
		fflush(stdout);
	}
	return out;
}

static bool break_payload() {
	return getenv("THICKET_BREAK_PAYLOAD") != nullptr;
}

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   iface({RNS::Type::NONE});
static RNS::Identity    identity({RNS::Type::NONE});
static RNS::Destination local_dest({RNS::Type::NONE});

static volatile bool payload_ok      = false;
static volatile bool payload_seen    = false;
static volatile bool proof_validated = false;
static RNS::Bytes    peer_dest_hash;   // Identity::recall() keys on the
                                       // DESTINATION hash, not the identity's
static volatile bool peer_known      = false;

// --- receiver -------------------------------------------------------------

// A free function, not a class: Destination::set_packet_callback takes
// void(*)(const Bytes&, const Packet&).
static void on_local_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
	(void)packet;
	payload_seen = true;
	const RNS::Bytes expect = make_payload();
	payload_ok = (data.size() == expect.size() &&
	              memcmp(data.data(), expect.data(), expect.size()) == 0);
	printf("[receiver] got %u bytes, match=%s\n",
	       (unsigned)data.size(), payload_ok ? "yes" : "NO");
	fflush(stdout);
}

// --- originator: learn the peer from its announce -------------------------

class Hearer : public RNS::AnnounceHandler {
public:
	Hearer() : RNS::AnnounceHandler(APP_NAME "." ASPECT) {}
	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) {
		(void)announced_identity;
		(void)app_data;
		if (peer_known) return;
		// Keep the DESTINATION hash: Identity::recall(destination_hash) is what
		// resolves it later. Passing the identity's own hash returns nothing and
		// the originator waits forever for a peer it has already heard.
		peer_dest_hash = destination_hash;
		peer_known = true;
		printf("[originator] heard our peer: %s\n",
		       destination_hash.toHex().c_str());
		fflush(stdout);
	}
};
static RNS::HAnnounceHandler hearer(new Hearer());

// --------------------------------------------------------------------------

static double deadline_s() {
	const char* t = getenv("THICKET_INTEROP_TIMEOUT_S");
	return t ? atof(t) : 40.0;
}

int main() {
	const char* role = getenv("THICKET_ROLE");
	if (!role || (strcmp(role, "originator") && strcmp(role, "receiver"))) {
		printf("[cpp] FAIL THICKET_ROLE must be 'originator' or 'receiver'\n");
		return 2;
	}
	const bool originating = (strcmp(role, "originator") == 0);
	printf("[%s] two-node starting\n", role);

	static microStore::Adapters::UniversalFileSystem filesystem;
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	iface = new UDPInterface2("seg",
	                          originating ? ORIG_LOCAL  : RECV_LOCAL,
	                          originating ? ORIG_REMOTE : RECV_REMOTE);
	iface.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(iface);
	iface.start();

	reticulum = RNS::Reticulum();
	reticulum.transport_enabled(false);   // both are leaves; see the header
	reticulum.start();

	identity = RNS::Identity();
	local_dest = RNS::Destination(identity, RNS::Type::Destination::IN,
	                              RNS::Type::Destination::SINGLE,
	                              APP_NAME, ASPECT);
	local_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

	if (!originating) local_dest.set_packet_callback(on_local_packet);
	else RNS::Transport::register_announce_handler(hearer);

	const double t0 = RNS::Utilities::OS::time();
	const double limit = deadline_s();

	// The receiver announces so the originator can learn it. The originator
	// never announces: it has nothing the peer needs, and a silent originator
	// makes "the receiver decrypted it" unambiguous -- there is only one
	// identity in play that could have encrypted anything.
	double last_announce = 0.0;
	bool   sent = false;
	double sent_at = 0.0;
	RNS::Packet outbound({RNS::Type::NONE});

	while (RNS::Utilities::OS::time() - t0 < limit) {
		RNS::Transport::loop();
		reticulum.loop();

		const double now = RNS::Utilities::OS::time();

		if (!originating && now - last_announce > 2.0) {
			local_dest.announce();
			last_announce = now;
		}

		if (originating && peer_known && !sent) {
			// Build the OUT side from the identity we just heard announced --
			// not from anything shared through the filesystem or a constant.
			// Learning the peer over the wire is part of what is being tested.
			RNS::Identity peer = RNS::Identity::recall(peer_dest_hash);
			if (!peer) { continue; }
			RNS::Destination to(peer, RNS::Type::Destination::OUT,
			                    RNS::Type::Destination::SINGLE,
			                    APP_NAME, ASPECT);
			outbound = RNS::Packet(to, make_payload(break_payload()));
			outbound.send();
			sent = true;
			sent_at = now;
			printf("[originator] sent %u bytes, awaiting proof\n",
			       (unsigned)make_payload(break_payload()).size());
			fflush(stdout);
		}

		if (originating && sent && !proof_validated) {
			RNS::PacketReceipt r = outbound.receipt();
			if (r && r.status() == RNS::Type::PacketReceipt::DELIVERED) {
				proof_validated = true;
				printf("[originator] proof validated after %.1fs\n",
				       now - sent_at);
				fflush(stdout);
			}
		}

		if (originating  && proof_validated) break;
		if (!originating && payload_seen && now - t0 > 6.0) break;
	}

	if (originating) {
		check("learned peer from announce", peer_known,
		      peer_known ? "identity recalled from the wire"
		                 : "never heard the receiver announce");
		check("sent", sent, sent ? "packet transmitted" : "never sent");
		// The load-bearing one for this role. A proof that validates means the
		// receiver decrypted our packet AND we accepted its answer -- both
		// halves of the round trip, between two instances of our own stack.
		check("proof validated", proof_validated,
		      proof_validated ? "our decode of our encode, confirmed by proof"
		                      : "no valid proof returned");
	}
	else {
		check("received", payload_seen,
		      payload_seen ? "packet arrived" : "nothing arrived");
		check("payload byte-identical", payload_ok,
		      payload_ok ? "NUL, 0xFF and UTF-8 survived intact"
		                 : "payload differs from what was sent");
	}

	int rc = 0;
	if (failures > 0) {
		printf("[%s] FAIL: %d of %d checks failed\n", role, failures, checks);
		rc = 1;
	}
	else {
		printf("[%s] SUCCESS all %d checks passed (two of our own nodes; "
		       "NOT a conformance result)\n", role, checks);
	}
	printf("[%s] exit code %d\n", role, rc);
	return rc;
}
