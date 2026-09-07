// Thicket interop scenario 4: LINK INBOUND, KEEPALIVE AND TEARDOWN UNDER LOSS.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// microReticulum's link scenario is C++ -> Python only: the C++ side builds
// the Link and tears it down. Nothing tested a Python peer establishing a Link
// TO a C++ destination, which is what a handheld is on the receiving end of.
//
// This binary is the responder. It announces, accepts an incoming Link,
// validates data over it, and reports the link's status over the run. It never
// initiates.
//
// The Python side drives the interesting part: it shortens its own keepalive
// and stale timers (both are per-Link attributes in the reference), idles the
// link long enough that it can only survive if THIS side answers keepalives,
// then cuts the wire through a UDP relay it controls and watches its own
// watchdog time the link out.
//
// LINK WATCHDOG. microReticulum now has one: Link::__watchdog_job() runs
// cooperatively from Transport::jobs() (there are no threads on the device).
// This side is the responder, so it does not originate keepalives, it answers
// them; but the stale-detection half of the watchdog still fires. It sets a
// short keepalive/stale window after establishment (3/12s, matching the Python
// side) so the close lands inside the run, and asserts that once the Python
// side cuts the wire the link is closed as TIMEOUT rather than left hanging.
//
// Exit 0 iff the link was established by the peer, data over it validated, and
// the watchdog closed the silent link as timed out.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>

#include <microReticulum.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static const char* APP_NAME = "thicket_interop";
static const char* ASPECT   = "link";

// Sent by Python over the established link. Under ENCRYPTED_MDU for a link
// (Link MDU is smaller than a Destination's), so 200 bytes.
#define LINK_PAYLOAD_LEN 200
static RNS::Bytes build_expected_payload() {
	RNS::Bytes p;
	uint32_t x = 0x11c0ffee;
	for (size_t i = 0; i < LINK_PAYLOAD_LEN; ++i) {
		x = (uint32_t)(x * 1664525u + 1013904223u);
		p.append((uint8_t)((x >> 16) & 0xff));
	}
	return p;
}

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   udp_interface(RNS::Type::NONE);
static RNS::Identity    local_identity({RNS::Type::NONE});
static RNS::Destination local_destination({RNS::Type::NONE});
static RNS::Link        inbound_link({RNS::Type::NONE});

static volatile bool link_established = false;
static volatile bool link_closed      = false;
static volatile bool data_ok          = false;
static volatile bool data_mismatch    = false;
static uint8_t       close_reason     = 0;

static int checks = 0, failures = 0, divergences = 0;

static void check(bool ok, const char* what, const std::string& detail) {
	++checks;
	if (ok) printf("[cpp]   OK   %-22s %s\n", what, detail.c_str());
	else  { ++failures; printf("[cpp]   FAIL %-22s %s\n", what, detail.c_str()); }
}

static bool bytes_equal(const RNS::Bytes& a, const RNS::Bytes& b) {
	return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

static void on_link_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
	(void)packet;
	const RNS::Bytes expected = build_expected_payload();
	const bool ok = bytes_equal(data, expected);
	printf("[cpp] link data: %lu bytes, match=%s\n",
	       (unsigned long)data.size(), ok ? "yes" : "no");
	if (ok) {
		data_ok = true;
		// Echo it back so the Python side can confirm the link carries data
		// in both directions, not just inbound.
		try {
			RNS::Packet(inbound_link, data).send();
			printf("[cpp] echoed %lu bytes back over the link\n",
			       (unsigned long)data.size());
		}
		catch (const std::exception& e) {
			printf("[cpp] echo failed: %s\n", e.what());
		}
	}
	else {
		data_mismatch = true;
		printf("[cpp] first 16 got:      %s\n", data.left(16).toHex().c_str());
		printf("[cpp] first 16 expected: %s\n", expected.left(16).toHex().c_str());
	}
	fflush(stdout);
}

static void on_link_closed_cb(RNS::Link& link) {
	link_closed = true;
	close_reason = (uint8_t)link.teardown_reason();
	printf("[cpp] link closed, teardown_reason=%u\n", (unsigned)close_reason);
	fflush(stdout);
}

static void on_link_established_cb(RNS::Link& link) {
	link_established = true;
	inbound_link = link;
	inbound_link.set_packet_callback(on_link_packet);
	inbound_link.set_link_closed_callback(on_link_closed_cb);
	// Short keepalive/stale so the watchdog's stale close lands inside the run
	// window; RNS ships 360/720s. These match the Python side's 3/12s. Set
	// after establishment: the reference recomputes them from RTT there, and
	// though this port does not, setting here is correct either way.
	inbound_link.keepalive(3);
	inbound_link.stale_time(12);
	printf("[cpp] inbound link established: %s (keepalive=%us stale=%us)\n",
	       link.hash().toHex().c_str(), inbound_link.keepalive(), inbound_link.stale_time());
	fflush(stdout);
}

int main() {
	printf("[cpp] link_inbound_responder starting\n");

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
	local_destination.set_link_established_callback(on_link_established_cb);
	local_destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

	local_destination.announce();
	printf("[cpp] announced own destination %s\n",
	       local_destination.hash().toHex().c_str());
	fflush(stdout);

	double TIMEOUT_S = 90.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double start   = RNS::Utilities::OS::time();
	double last_announce = start;

	// Run for the full window on purpose. The Python side needs us alive after
	// it cuts the wire, so that "did the C++ side notice" is a question the run
	// can actually answer.
	while (true) {
		reticulum.loop();
		const double now = RNS::Utilities::OS::time();
		if (now - start > TIMEOUT_S) break;
		if (now - last_announce >= 2.0 && !link_established) {
			local_destination.announce();
			last_announce = now;
		}
		RNS::Utilities::OS::sleep(0.01);
	}

	RNS::Transport::deregister_interface(udp_interface);

	printf("[cpp] --- results ---\n");
	check(link_established, "peer established link",
	      link_established ? "yes" : "no incoming link arrived");
	check(data_ok && !data_mismatch, "link data validated",
	      data_mismatch ? "payload mismatch"
	                    : (data_ok ? "200 bytes matched" : "no data arrived"));

	// --- LINK WATCHDOG ------------------------------------------------------
	// The Python side cut the wire past this link's stale window. The watchdog
	// must have noticed the silence and closed the link as timed out. Before
	// the watchdog existed this was a known divergence (the link hung open);
	// now it is a required check, and the reason to have it: a field device
	// that never times out a silent peer locks up in exactly the adverse
	// conditions it is built for.
	check(link_closed && close_reason == (uint8_t)RNS::Type::Link::TIMEOUT,
	      "silent link timed out",
	      !link_closed
	          ? "link never closed -- watchdog did not fire on a silent peer"
	          : (close_reason == (uint8_t)RNS::Type::Link::TIMEOUT
	                 ? "watchdog closed the link as timed out"
	                 : "closed, but not by timeout (peer gave up earlier)"));

	int rc = 0;
	if (failures > 0) {
		printf("[cpp] FAIL: %d of %d checks failed\n", failures, checks);
		rc = 1;
	}
	else {
		printf("[cpp] SUCCESS %d of %d link checks passed; %d known divergence(s) "
		       "still present\n", checks - divergences, checks, divergences);
	}
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
