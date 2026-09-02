// Thicket interop scenario 1: COLD INBOUND.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// Every microReticulum interop scenario has the C++ side speak first: it
// announces, hears Python's announce, and transmits. Python only ever
// answers. Nothing tested a Python peer originating to a device that hasn't
// just transmitted to it -- which is what a handheld does all day.
//
// This binary is therefore a pure receiver. It:
//
//   1. Announces its SINGLE destination, and nothing else.
//   2. NEVER constructs an OUT destination, and never transmits anything
//      other than announces and the automatic PROVE_ALL proof.
//   3. Waits for a packet originated by Python, decrypts it, and validates
//      the plaintext against a fixed 383-byte pattern (RNS 1.4.2's
//      ENCRYPTED_MDU -- the largest a single encrypted packet can carry).
//   4. Folds a digest of the decrypted plaintext into the app_data of its
//      subsequent announces, so the Python side can verify the decryption
//      independently rather than taking our word for it.
//
// Coldness is structural, not a comment: the Python side of this scenario
// doesn't announce at all, so this process can't learn its identity and
// can't address it. As a guard against the scenario silently decaying into
// the warm case, we count foreign announces and fail if any arrives before
// the packet does -- if Python ever starts announcing, this test stops
// claiming to be cold.
//
// Exit 0 iff: a packet arrived, decrypted, matched the expected pattern, and
// no foreign announce preceded it.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>

#include <microReticulum.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* APP_NAME = "thicket_interop";
static const char* ASPECT   = "cold";

// 383 bytes: RNS 1.4.2's ENCRYPTED_MDU, i.e. the largest payload that fits in
// a single encrypted SINGLE-destination packet. Using the maximum rather than
// a round number means an off-by-one anywhere in the C++ decrypt path shows up
// here rather than in the field.
//
// Deliberately not a 0x00..0xff ramp: a ramp is what you would also get from
// an uninitialised buffer that happened to look right. This is an LCG, so
// every byte depends on the one before it and any truncation, reordering or
// single-byte corruption is visible.
#define COLD_PAYLOAD_LEN 383
static RNS::Bytes build_expected_payload() {
	RNS::Bytes p;
	uint32_t x = 0x5eed1e5f;
	for (size_t i = 0; i < COLD_PAYLOAD_LEN; ++i) {
		x = (uint32_t)(x * 1664525u + 1013904223u);
		p.append((uint8_t)((x >> 24) & 0xff));
	}
	return p;
}

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   udp_interface(RNS::Type::NONE);
static RNS::Identity    local_identity({RNS::Type::NONE});
static RNS::Destination local_destination({RNS::Type::NONE});

static volatile bool received_ok       = false;
static volatile bool received_mismatch = false;
static volatile int  foreign_announces = 0;

static RNS::Bytes ack_app_data;

static bool bytes_equal(const RNS::Bytes& a, const RNS::Bytes& b) {
	return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

// The proof we emit under PROVE_ALL is NOT evidence of decryption: Transport
// proves a packet after handing it to the destination, whether or not the
// destination could decrypt it (Transport.cpp, DATA branch) -- and the Python
// reference does the same, so this is parity, not a bug. It does mean a
// PacketReceipt reaching DELIVERED tells the sender nothing about plaintext.
//
// So that the Python side can independently verify we decrypted the exact
// bytes, we fold a digest of the decrypted plaintext into the app_data of a
// subsequent announce. An announce is something a pure receiver legitimately
// emits, so this keeps the scenario cold while giving the reference
// implementation something checkable.
// The 17th byte is the hop count the packet carried when it reached us, and
// it's what lets one binary serve both the direct and the relayed topology.
// Transport::inbound increments hops before dispatch (Transport.cpp), so a
// directly-received packet reports 1 and each transport node adds one more.
// The Python side pins the exact value it expects for its topology, so a
// packet that silently took the wrong path fails the scenario instead of
// passing it -- without that byte, a broken relay that somehow delivered
// direct would look identical to success.
static RNS::Bytes build_ack_app_data(const RNS::Bytes& plaintext, uint8_t hops) {
	RNS::Bytes material(plaintext);
	material.append("thicket-cold-ack");
	RNS::Bytes out = RNS::Identity::full_hash(material).left(16);
	out.append(hops);
	return out;
}

static void on_local_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
	const uint8_t hops = packet.hops();
	const RNS::Bytes expected = build_expected_payload();
	const bool ok = bytes_equal(data, expected);
	printf("[cpp] inbound packet decrypted: %lu bytes (expected %lu), match=%s, "
	       "hops=%u\n",
	       (unsigned long)data.size(), (unsigned long)expected.size(),
	       ok ? "yes" : "no", (unsigned)hops);
	if (ok) {
		received_ok = true;
		ack_app_data = build_ack_app_data(data, hops);
		printf("[cpp] ack app_data (sha256(plaintext||tag)[:16] || hops) = %s\n",
		       ack_app_data.toHex().c_str());
	}
	else {
		received_mismatch = true;
		printf("[cpp] first 32 bytes got:      %s\n",
		       data.left(32).toHex().c_str());
		printf("[cpp] first 32 bytes expected: %s\n",
		       expected.left(32).toHex().c_str());
	}
}

// Coldness guard. This handler exists ONLY to detect the scenario degrading
// into the warm case. It never sends.
class ColdnessGuard : public RNS::AnnounceHandler {
public:
	ColdnessGuard()
	  : RNS::AnnounceHandler((std::string(APP_NAME) + "." + ASPECT).c_str()) {}

	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		(void)announced_identity;
		(void)app_data;
		// Our own announce echoed back by the loopback path isn't foreign.
		if (destination_hash == local_destination.hash()) return;
		foreign_announces++;
		printf("[cpp] UNEXPECTED foreign announce from %s -- this scenario is "
		       "supposed to be cold; the peer must not announce\n",
		       destination_hash.toHex().c_str());
	}
};
static RNS::HAnnounceHandler coldness_guard(new ColdnessGuard());

int main() {
	printf("[cpp] cold_inbound_receiver starting\n");

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	udp_interface = new UDPInterface();
	udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(udp_interface);
	udp_interface.start();

	reticulum = RNS::Reticulum();
	reticulum.transport_enabled(false);
	reticulum.start();

	local_identity = RNS::Identity();
	local_destination = RNS::Destination(local_identity,
	                                     RNS::Type::Destination::IN,
	                                     RNS::Type::Destination::SINGLE,
	                                     APP_NAME, ASPECT);
	local_destination.set_packet_callback(on_local_packet);
	// PROVE_ALL: the proof we emit is the only thing we transmit besides
	// announces, and it's what lets the Python side independently confirm
	// delivery by validating our signature.
	local_destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

	RNS::Transport::register_announce_handler(coldness_guard);

	local_destination.announce();
	printf("[cpp] announced own destination %s\n",
	       local_destination.hash().toHex().c_str());
	printf("[cpp] identity public key %s\n",
	       local_identity.get_public_key().toHex().c_str());
	fflush(stdout);

	// Kept in step with the driver's watchdog so a failure ends in this
	// process's own TIMEOUT diagnostic and exit 1, rather than in a SIGTERM
	// that says nothing about what went wrong.
	double TIMEOUT_S = 40.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double LINGER_S       = 4.0;   // keep looping after receipt so the
	                                     // proof and the ack announce leave.
	const double start          = RNS::Utilities::OS::time();
	double last_announce        = start;
	double received_at          = 0.0;

	while (true) {
		reticulum.loop();
		const double now = RNS::Utilities::OS::time();

		if ((received_ok || received_mismatch) && received_at == 0.0) {
			received_at = now;
		}
		if (received_at != 0.0 && now - received_at >= LINGER_S) break;

		if (now - start > TIMEOUT_S) {
			printf("[cpp] TIMEOUT (received_ok=%d mismatch=%d foreign_announces=%d)\n",
			       received_ok ? 1 : 0, received_mismatch ? 1 : 0,
			       foreign_announces);
			break;
		}

		if (now - last_announce >= 1.5) {
			if (received_ok) {
				// Post-decryption announces carry the plaintext digest, which
				// is the Python side's only independent evidence that we
				// decrypted the bytes it actually sent.
				local_destination.announce(ack_app_data);
				printf("[cpp] announced ack app_data %s\n",
				       ack_app_data.toHex().c_str());
			}
			else if (!received_mismatch) {
				// Keep announcing: this is the only way the Python side can
				// learn our destination, and the first announce may race its
				// startup.
				local_destination.announce();
				printf("[cpp] re-announced\n");
			}
			last_announce = now;
			fflush(stdout);
		}

		RNS::Utilities::OS::sleep(0.01);
	}

	RNS::Transport::deregister_interface(udp_interface);

	int rc = 0;
	if (!received_ok)             { printf("[cpp] FAIL: no matching inbound packet was decrypted\n"); rc = 1; }
	if (received_mismatch)        { printf("[cpp] FAIL: inbound payload did not match\n");            rc = 1; }
	if (foreign_announces > 0)    { printf("[cpp] FAIL: %d foreign announce(s) seen; not a cold test\n", foreign_announces); rc = 1; }

	if (rc == 0) printf("[cpp] SUCCESS cold inbound packet received and validated\n");
#ifdef THICKET_POOL_PROBE
	// Off by default so the scenario's own behaviour is untouched. Built with
	// the pool allocator and the device's own pool size, this answers "is
	// 65536 the right number" from the wrong word size but in the safe
	// direction: every pointer and size_t here is twice its width on the
	// nRF52840, so a peak that fits at 64-bit can't fail to fit at 32-bit.
	// It's an upper bound, not the figure -- the real one needs the board.
	{
		const size_t pool = RNS::Utilities::Memory::heap_pool_size();
		const size_t freen = RNS::Utilities::Memory::heap_pool_free();
		printf("[cpp] POOL size=%lu free=%lu peak_used=%lu frag=%u%%\n",
		       (unsigned long)pool, (unsigned long)freen,
		       (unsigned long)(pool - freen),
		       (unsigned)RNS::Utilities::Memory::heap_pool_fragmented());
		RNS::Utilities::Memory::dump_basic_pool_stats();
	}
#endif
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
