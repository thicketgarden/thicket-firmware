// Thicket interop scenario: NOMADNET PAGE FETCH, C++ REQUESTER.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// The desktop page browser fetches a Nomad Network page by establishing a Link
// to a node and calling Link::request("/page/...") over it. Nothing in this
// suite exercised that path. `Link::request` exists at our pin, but existence
// is not proof: the same pin ships an empty Link watchdog. A capability the
// browser depends on belongs under continuous test, so this scenario proves the
// whole round trip against the reference: discover a Python NomadNet node by its
// announce, establish a Link, request a known page, and verify the Micron comes
// back byte-for-byte.
//
// This is the browser's fetch edge, reduced to the wire. The C++ side is the
// requester, exactly as the handheld and the desktop browser are; the Python
// side is a stock RNS node serving a page through a registered request handler,
// exactly as a real NomadNet node does.
//
// TWO DELIBERATE BOUNDS, both mirroring the reference client documented in
// microReticulum's own examples/nomadnet:
//   * The page is small, so the response is a single-packet RPC and not a
//     Resource. Standard NomadNet nodes bz2-compress a Resource-sized response,
//     and microReticulum has no bz2, so a large page is not retrievable at this
//     pin. That bound belongs to the browser, not to this test, so the test
//     stays on the single-packet path rather than working around it.
//   * A page fetch carries no request body, which on the wire is msgpack nil
//     (0xC0), not zero bytes: the request envelope is a 3-element msgpack array
//     and the reference's unpackb rejects a short one.
//
// Exit 0 and print SUCCESS iff the node was discovered, the Link established,
// and the returned page equalled the known page.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>
#include <MsgPack.h>

#include <microReticulum.h>

#include <stdio.h>
#include <string.h>
#include <string>

// NomadNet aspect: the full aspect string is "nomadnetwork.node", matching
// nomadnet/Node.py, so this requester is wire-compatible with a real node.
static const char* APP_NAME  = "nomadnetwork";
static const char* ASPECT    = "node";
static const char* PAGE_PATH = "/page/index.mu";

// The known page, defined identically on both sides. Real Micron: a heading, a
// line of prose, a link. Kept well under the link MDU so the response is a
// single packet and never becomes a compressed Resource.
static const char* EXPECTED_PAGE =
	">Interop Test Node\n"
	"\n"
	"This page proves the C++ page-fetch lifecycle end to end.\n"
	"\n"
	"`[Home`:/page/index.mu]\n";

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   udp_interface(RNS::Type::NONE);
static RNS::Identity    client_identity({RNS::Type::NONE});
static RNS::Destination outgoing_destination({RNS::Type::NONE});
static RNS::Link        active_link({RNS::Type::NONE});

static volatile bool announce_seen     = false;
static volatile bool link_up           = false;
static volatile bool response_received = false;
static volatile bool response_ok       = false;
static volatile bool request_failed    = false;

// Unwrap a msgpack `bin` value. The response microReticulum delivers is the
// still-encoded `response` element of the server's [request_id, response]
// envelope; a NomadNet page is served as Python bytes, which is always msgpack
// `bin`. Same pattern as microReticulum/examples/nomadnet.
static bool msgpack_unwrap_bin(const RNS::Bytes& packed, RNS::Bytes& out) {
	if (packed.size() < 1) return false;
	MsgPack::Unpacker u;
	u.feed(packed.data(), packed.size());
	if (!u.isBin()) return false;
	MsgPack::bin_t<uint8_t> bin;
	u.deserialize(bin);
	out = RNS::Bytes(bin.data(), bin.size());
	return true;
}

static void on_response(const RNS::RequestReceipt& receipt) {
	RNS::Bytes encoded = const_cast<RNS::RequestReceipt&>(receipt).get_response();
	response_received = true;
	// A single-packet response arrives as the raw page bytes; a larger one can
	// arrive still msgpack-bin-wrapped. Accept either: unwrap if it is a bin
	// value, otherwise take the bytes as-is.
	RNS::Bytes page = encoded;
	RNS::Bytes unwrapped;
	if (msgpack_unwrap_bin(encoded, unwrapped)) page = unwrapped;
	const RNS::Bytes expected((const uint8_t*)EXPECTED_PAGE, strlen(EXPECTED_PAGE));
	response_ok = (page.size() == expected.size() &&
	               memcmp(page.data(), expected.data(), page.size()) == 0);
	printf("[cpp] page received: %lu bytes, match=%s\n",
	       (unsigned long)page.size(), response_ok ? "yes" : "no");
	if (!response_ok) {
		printf("[cpp] got:      %s\n", page.left(24).toHex().c_str());
		printf("[cpp] expected: %s\n", expected.left(24).toHex().c_str());
	}
	fflush(stdout);
}

static void on_failed(const RNS::RequestReceipt& receipt) {
	(void)receipt;
	request_failed = true;
	printf("[cpp] request failed (timeout, missing handler, or a compressed "
	       "response the C++ port cannot decompress)\n");
	fflush(stdout);
}

static void on_link_closed(RNS::Link& link) {
	(void)link;
	printf("[cpp] link closed\n");
	fflush(stdout);
}

static void on_link_established(RNS::Link& link) {
	link_up = true;
	printf("[cpp] link established: %s\n", link.hash().toHex().c_str());

	// Identify to the node over the encrypted link. Not required for an
	// ALLOW_ALL page, but harmless and matches a real client.
	link.identify(client_identity);

	// A page fetch has no body. On the wire that is msgpack nil, one byte, not
	// zero bytes: the request envelope is a 3-element array and a short one is
	// rejected by the reference.
	static const uint8_t MSGPACK_NIL = 0xC0;
	const RNS::Bytes nil_payload(&MSGPACK_NIL, 1);
	printf("[cpp] requesting %s\n", PAGE_PATH);
	fflush(stdout);
	link.request(RNS::Bytes(PAGE_PATH), nil_payload,
	             on_response, on_failed, /*progress=*/nullptr, /*timeout=*/10.0);
}

class AnnounceHandler : public RNS::AnnounceHandler {
public:
	AnnounceHandler() : RNS::AnnounceHandler(
	    (std::string(APP_NAME) + "." + ASPECT).c_str()) {}
	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		if (announce_seen) return;
		announce_seen = true;
		printf("[cpp] discovered node by announce: %s (\"%s\")\n",
		       destination_hash.toHex().c_str(),
		       app_data ? app_data.toString().c_str() : "");
		fflush(stdout);
		outgoing_destination = RNS::Destination(announced_identity,
		                                        RNS::Type::Destination::OUT,
		                                        RNS::Type::Destination::SINGLE,
		                                        APP_NAME, ASPECT);
		active_link = RNS::Link(outgoing_destination,
		                        on_link_established, on_link_closed);
	}
};
static RNS::HAnnounceHandler announce_handler(new AnnounceHandler());

int main() {
	printf("[cpp] page_fetch_requester starting\n");

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

	client_identity = RNS::Identity();
	RNS::Transport::register_announce_handler(announce_handler);

	double TIMEOUT_S = 30.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double start = RNS::Utilities::OS::time();

	while (true) {
		reticulum.loop();
		if (response_received || request_failed) {
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED)
				active_link.teardown();
			break;
		}
		if (RNS::Utilities::OS::time() - start > TIMEOUT_S) {
			printf("[cpp] TIMEOUT (announce=%d link=%d resp=%d failed=%d)\n",
			       announce_seen ? 1 : 0, link_up ? 1 : 0,
			       response_received ? 1 : 0, request_failed ? 1 : 0);
			break;
		}
		RNS::Utilities::OS::sleep(0.01);
	}

	// Let the teardown packet leave the wire.
	const double cleanup_until = RNS::Utilities::OS::time() + 0.5;
	while (RNS::Utilities::OS::time() < cleanup_until) {
		reticulum.loop();
		RNS::Utilities::OS::sleep(0.01);
	}
	RNS::Transport::deregister_interface(udp_interface);

	printf("[cpp] --- results ---\n");
	printf("[cpp]   %s node discovered by announce\n", announce_seen ? "OK  " : "FAIL");
	printf("[cpp]   %s link established\n", link_up ? "OK  " : "FAIL");
	printf("[cpp]   %s page returned and matched\n", response_ok ? "OK  " : "FAIL");

	const int rc = (announce_seen && link_up && response_ok) ? 0 : 1;
	if (rc == 0) printf("[cpp] SUCCESS page fetched and verified over a Link\n");
	else         printf("[cpp] FAIL page fetch did not complete\n");
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
