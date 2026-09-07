// Thicket interop scenario: NOMADNET PAGE FETCH, C++ REQUESTER.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The desktop page browser fetches a Nomad Network page by establishing a Link
// to a node and calling Link::request("/page/..."). This proves the whole round
// trip against the reference, including the two things that make a real page
// hard: it arrives as a multi-packet Resource that is msgpack-wrapped, and the
// reference bz2-compresses it.
//
// The page is a real one, `desktop/pages/index.mu` (564 bytes), read by both
// sides. Three requests over one Link, asserting the current behaviour of the
// stack:
//   1. /page/index.mu, auto_compress=False. Uncompressed multi-packet Resource.
//      Must succeed and match byte-for-byte (Resource assembly + msgpack decode).
//   2. /page/gz.mu, the same page, auto_compress=True. RNS bz2-compresses it, so
//      it arrives as a compressed Resource. Must succeed: the capped bz2
//      decompressor assembles it and the page matches. This is the capability
//      that lets the handheld browse standard nodes.
//   3. /page/big.mu, a page over the on-device size cap (RNS_BUNZIP_CAP, 24 KB),
//      compressed. Must FAIL CLEANLY: the decompressor refuses a block that
//      would exceed the cap, the resource is concluded CORRUPT, and the request
//      fails rather than hanging. This is the cap, and the watchdog and
//      resource-conclusion fixes that make an over-cap page a bounded failure.
//
// A page fetch carries no request body, which on the wire is msgpack nil (0xC0),
// not zero bytes: the request envelope is a 3-element array the reference rejects
// if short.
//
// Exit 0 and print SUCCESS iff the node was discovered, the Link established, the
// uncompressed and the compressed pages both returned and matched, and the
// over-cap page failed cleanly.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>
#include <MsgPack.h>

#include <microReticulum.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static const char* APP_NAME   = "nomadnetwork";
static const char* ASPECT     = "node";
static const char* PATH_PLAIN = "/page/index.mu";  // uncompressed
static const char* PATH_GZIP  = "/page/gz.mu";     // compressed, under the cap
static const char* PATH_BIG   = "/page/big.mu";    // compressed, over the cap

// The small page, loaded from THICKET_PAGE_FILE, identical to what the server
// serves at index.mu and gz.mu.
static RNS::Bytes expected_page;

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   udp_interface(RNS::Type::NONE);
static RNS::Identity    client_identity({RNS::Type::NONE});
static RNS::Destination outgoing_destination({RNS::Type::NONE});
static RNS::Link        active_link({RNS::Type::NONE});

enum Phase { PH_PLAIN = 0, PH_GZIP = 1, PH_BIG = 2, PH_DONE = 3 };
static volatile int phase = PH_PLAIN;

static volatile bool announce_seen = false;
static volatile bool link_up       = false;

static volatile bool plain_done = false, plain_ok = false;
static volatile bool gz_done = false, gz_ok = false, gz_unwrapped = false;
static volatile bool big_done = false, big_rejected = false, big_unexpected = false, big_hung = false;

// Unwrap a msgpack `bin` value. A Resource response is delivered still
// msgpack-encoded; a page served as bytes is always msgpack `bin`.
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

static bool matches_expected(const RNS::Bytes& encoded, bool& used_unwrap) {
	RNS::Bytes page = encoded, unwrapped;
	used_unwrap = false;
	if (msgpack_unwrap_bin(encoded, unwrapped)) { page = unwrapped; used_unwrap = true; }
	return page.size() == expected_page.size() &&
	       memcmp(page.data(), expected_page.data(), page.size()) == 0;
}

static void on_response(const RNS::RequestReceipt& receipt) {
	RNS::Bytes encoded = const_cast<RNS::RequestReceipt&>(receipt).get_response();
	bool uw = false;
	if (phase == PH_PLAIN) {
		plain_ok = matches_expected(encoded, uw);
		plain_done = true;
		printf("[cpp] /page/index.mu: %lu bytes, match=%s\n",
		       (unsigned long)encoded.size(), plain_ok ? "yes" : "no");
	} else if (phase == PH_GZIP) {
		gz_ok = matches_expected(encoded, uw);
		gz_unwrapped = uw;
		gz_done = true;
		printf("[cpp] /page/gz.mu: decompressed, match=%s (bz2 assembled on-device)\n",
		       gz_ok ? "yes" : "no");
	} else if (phase == PH_BIG) {
		big_unexpected = true;   // an over-cap page should never decompress
		big_done = true;
		printf("[cpp] /page/big.mu SUCCEEDED unexpectedly -- the size cap did not hold\n");
	}
	fflush(stdout);
}

static void on_failed(const RNS::RequestReceipt& receipt) {
	(void)receipt;
	if (phase == PH_PLAIN) { plain_done = true; printf("[cpp] /page/index.mu FAILED\n"); }
	else if (phase == PH_GZIP) { gz_done = true; printf("[cpp] /page/gz.mu FAILED (bz2 decompress did not complete)\n"); }
	else if (phase == PH_BIG) {
		big_rejected = true; big_done = true;
		printf("[cpp] /page/big.mu failed cleanly, as the over-cap reject predicts\n");
	}
	fflush(stdout);
}

static void request_phase() {
	static const uint8_t MSGPACK_NIL = 0xC0;
	const RNS::Bytes nil_payload(&MSGPACK_NIL, 1);
	const char* path = phase == PH_PLAIN ? PATH_PLAIN : (phase == PH_GZIP ? PATH_GZIP : PATH_BIG);
	printf("[cpp] requesting %s\n", path);
	fflush(stdout);
	active_link.request(RNS::Bytes(path), nil_payload,
	                    on_response, on_failed, nullptr, /*timeout=*/10.0);
}

static void on_link_closed(RNS::Link& link) { (void)link; }

static void on_link_established(RNS::Link& link) {
	link_up = true;
	printf("[cpp] link established: %s\n", link.hash().toHex().c_str());
	link.identify(client_identity);
	request_phase();
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

static bool load_page() {
	const char* file = getenv("THICKET_PAGE_FILE");
	if (!file) { printf("[cpp] THICKET_PAGE_FILE not set\n"); return false; }
	FILE* f = fopen(file, "rb");
	if (!f) { printf("[cpp] cannot open page file %s\n", file); return false; }
	char buf[4096]; size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) expected_page.append((uint8_t*)buf, n);
	fclose(f);
	printf("[cpp] page file %s: %lu bytes\n", file, (unsigned long)expected_page.size());
	return expected_page.size() > 0;
}

int main() {
	printf("[cpp] page_fetch_requester starting\n");
	if (!load_page()) { printf("[cpp] FAIL setup\n"); return 1; }

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

	double TIMEOUT_S = 45.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double start = RNS::Utilities::OS::time();

	// The over-cap request should fail fast (the resource is rejected on
	// assembly and the request is concluded); this backstop catches a
	// regression to a hang.
	static const double BIG_WAIT = 14.0;
	double big_deadline = 0.0;

	while (true) {
		reticulum.loop();
		const double now = RNS::Utilities::OS::time();

		if (phase == PH_PLAIN && plain_done) {
			phase = PH_GZIP;
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED) request_phase();
			else gz_done = true;
		}
		if (phase == PH_GZIP && gz_done) {
			phase = PH_BIG;
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED) { request_phase(); big_deadline = now + BIG_WAIT; }
			else big_done = true;
		}
		if (phase == PH_BIG && !big_done && big_deadline > 0.0 && now > big_deadline) {
			big_hung = true; big_done = true;
			printf("[cpp] /page/big.mu did not conclude in %.0fs (regression: the "
			       "over-cap reject should fail it)\n", BIG_WAIT);
		}
		if (big_done) {
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED) active_link.teardown();
			break;
		}
		if (now - start > TIMEOUT_S) {
			printf("[cpp] TIMEOUT (announce=%d link=%d plain=%d gz=%d big=%d)\n",
			       announce_seen, link_up, plain_done, gz_done, big_done);
			break;
		}
		RNS::Utilities::OS::sleep(0.01);
	}

	const double cleanup_until = RNS::Utilities::OS::time() + 0.5;
	while (RNS::Utilities::OS::time() < cleanup_until) { reticulum.loop(); RNS::Utilities::OS::sleep(0.01); }
	RNS::Transport::deregister_interface(udp_interface);

	printf("[cpp] --- results ---\n");
	printf("[cpp]   %s node discovered by announce\n", announce_seen ? "OK  " : "FAIL");
	printf("[cpp]   %s link established\n", link_up ? "OK  " : "FAIL");
	printf("[cpp]   %s uncompressed page returned and matched\n", plain_ok ? "OK  " : "FAIL");
	printf("[cpp]   %s compressed page decompressed on-device and matched (capped bz2)\n", gz_ok ? "OK  " : "FAIL");

	int failures = 0;
	if (!announce_seen) ++failures;
	if (!link_up) ++failures;
	if (!plain_ok) ++failures;
	if (!gz_ok) ++failures;

	if (big_rejected) {
		printf("[cpp]   OK   over-cap page rejected cleanly (failed, not hung, not decoded)\n");
	} else if (big_unexpected) {
		++failures;
		printf("[cpp]   FAIL over-cap page decompressed; the on-device size cap did not hold\n");
	} else {
		++failures;
		printf("[cpp]   FAIL over-cap page %s\n", big_hung ? "hung (no clean reject)" : "did not conclude");
	}

	const int rc = failures ? 1 : 0;
	if (rc == 0) printf("[cpp] SUCCESS uncompressed + compressed pages fetched, over-cap rejected cleanly\n");
	else         printf("[cpp] FAIL %d check(s) failed\n", failures);
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
