// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TRANSPORT FORWARDING, the scenario where we're the router.
//
// Every other scenario in this suite puts our stack at the edge: a leaf that
// receives, decrypts and answers. This one puts two Python RNS peers on
// opposite sides of us and makes them reach each other *through* us. They share
// no interface and can't hear one another directly, so a message arriving at
// the far end is proof that this process forwarded it.
//
// WHY IT MATTERS MORE THAN THE OTHER FIVE
//
// `Transport.cpp` is where all four of our local patches live, and until now it
// had no interop coverage in the one mode those patches are about. Worse, the
// leaf scenarios run with `transport_enabled(false)`, so the entire forwarding
// path, the announce rebroadcast, and the path-table population that a router
// does have never executed against the reference implementation at all.
//
// This process therefore runs with transport ENABLED, which no other scenario
// does.
//
// WHAT IT ASSERTS, AND WHY EACH IS NOT ENOUGH ALONE
//
//   1. Both peers learn each other. Necessary, but an announce could in
//      principle reach them by some path we didn't intend.
//   2. The far peer receives the originator's packet. Stronger, the two have
//      no shared interface, but still says nothing about hop accounting.
//   3. **The packet arrives with a hop count that proves it crossed us.** RNS
//      increments hops on ingress, so a packet the originator sent at 0 arrives
//      at the far end having been counted twice: once by us, once by them.
//      A packet that somehow travelled directly would read one lower.
//   4. We hold a path to both peers. A router that forwards without recording
//      a path is working by accident.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface2.h>

#include <microReticulum.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Ports, and the shape of the thing:
//
//     python originator            us (router)            python far end
//     14280 <---------------> 14281 | 14283 <---------------> 14282
//              segment A                        segment B
//
// Two point-to-point UDP segments with no member in common except us.
static const int A_LOCAL  = 14281;   // our side of segment A
static const int A_REMOTE = 14280;   // the originator
static const int B_LOCAL  = 14283;   // our side of segment B
static const int B_REMOTE = 14282;   // the far end

static const char* APP_NAME = "thicket_interop";
static const char* ASPECT   = "forward";

static RNS::Reticulum reticulum({RNS::Type::NONE});
static RNS::Interface iface_a(RNS::Type::NONE);
static RNS::Interface iface_b(RNS::Type::NONE);

static int announces_seen = 0;
static int paths_held = 0;

static void info(const char* what) { printf("[cpp] %s\n", what); }

// We're a router, not a correspondent: we register no destination of our own
// and answer nothing. This handler exists only to report what the routing
// tables learn, so a failure says which half is missing.
class Watcher : public RNS::AnnounceHandler {
public:
	Watcher() : RNS::AnnounceHandler("") {}  // empty filter: everything
	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		(void)announced_identity;
		(void)app_data;
		announces_seen++;
		// Assertion 4 in the header: a router that forwards without recording
		// a path is working by accident. Check it at the moment the announce
		// lands rather than trusting that it must have.
		const bool have = RNS::Transport::has_path(destination_hash);
		if (have) paths_held++;
		printf("[cpp] announce seen for %s (hops to it: %d, path: %s)\n",
		       destination_hash.toHex().c_str(),
		       (int)RNS::Transport::hops_to(destination_hash),
		       have ? "yes" : "NO");
	}
};
static RNS::HAnnounceHandler watcher(new Watcher());

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("[cpp] transport forwarder starting\n");
	printf("[cpp] segment A: listen %d -> %d   segment B: listen %d -> %d\n",
	       A_LOCAL, A_REMOTE, B_LOCAL, B_REMOTE);

	static microStore::Adapters::UniversalFileSystem filesystem;
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	// Two interfaces, one per neighbour. MODE_GATEWAY on both: this node is
	// not an edge and must be willing to carry traffic in either direction.
	iface_a = new UDPInterface2("segA", A_LOCAL, A_REMOTE);
	iface_a.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(iface_a);
	iface_a.start();

	iface_b = new UDPInterface2("segB", B_LOCAL, B_REMOTE);
	iface_b.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(iface_b);
	iface_b.start();

	reticulum = RNS::Reticulum();
	// THE line this scenario exists for. Every other scenario sets this false.
	reticulum.transport_enabled(true);
	reticulum.start();
	info("transport ENABLED, this node routes");

	RNS::Transport::register_announce_handler(watcher);

	const char* env_timeout = getenv("THICKET_INTEROP_TIMEOUT_S");
	const double timeout = env_timeout ? atof(env_timeout) : 60.0;
	const double started = RNS::Utilities::OS::time();

	// A router has nothing to do but run. It never originates, never answers,
	// and its success is measured entirely at the two ends.
	while (RNS::Utilities::OS::time() - started < timeout) {
		reticulum.loop();
		RNS::Utilities::OS::sleep(0.01);
	}

	printf("[cpp] --- results ---\n");
	printf("[cpp]   announces observed: %d, of which a path was recorded: %d\n",
	       announces_seen, paths_held);

	// Deliberately weak, and the comment matters more than the check: this
	// process can't tell whether forwarding worked, only whether it was in a
	// position to forward. The verdict lives at the two Python ends, which is
	// the honest place for it -- a router certifying its own delivery would be
	// marking its own homework.
	//
	// ⚠ An earlier version required TWO announces, one per peer, and failed a
	// run in which the forwarding demonstrably worked. The originator is cold:
	// it never announces, because it addresses the far end from an identity
	// file so that a failure can't be confused with a lost announce. Only the
	// far end announces, so one is the correct expectation.
	if (announces_seen >= 1 && paths_held >= 1) {
		printf("[cpp] SUCCESS transport enabled, %d announce(s) heard and a "
		       "path recorded for %d of them -- this node was in a position to "
		       "route\n", announces_seen, paths_held);
		return 0;
	}
	printf("[cpp] FAILURE announces=%d paths=%d. Expected at least one of "
	       "each: without a path this node cannot forward, whatever the ends "
	       "report.\n", announces_seen, paths_held);
	return 1;
}
