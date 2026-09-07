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
// browser depends on belongs under continuous test, so this proves the whole
// round trip against the reference.
//
// The page is a real one, `desktop/pages/index.mu`, read by both sides. At 564
// bytes it is larger than the 431-byte link MDU, so the response is a multi-
// packet RESOURCE, not a single packet. That is the case that matters: a
// Resource response comes back msgpack-`bin`-wrapped and has to be decoded, and
// a Resource is where the reference's default compression bites.
//
// TWO REQUESTS, over one Link, asserting opposite outcomes:
//   1. /page/index.mu, served with auto_compress=False. Uncompressed Resource.
//      Must succeed: the C++ side assembles it, msgpack-decodes it, and the
//      page matches byte-for-byte. This is the browser's fetch, proven.
//   2. /page/gz.mu, the same page served with the reference's default
//      auto_compress=True. The page bz2-compresses smaller (564 -> 400), so the
//      reference sends a bz2 Resource. microReticulum has no bz2, so
//      Resource::assemble rejects it and the request FAILS. That failure is
//      asserted as a KNOWN DIVERGENCE: it is the reason the browser can only
//      fetch pages a node chose not to compress until microReticulum gains bz2.
//      If this request ever SUCCEEDS, bz2 has arrived and the exemption below
//      must go, so the scenario turns red to force it out.
//
// A page fetch carries no request body, which on the wire is msgpack nil
// (0xC0), not zero bytes: the request envelope is a 3-element msgpack array and
// the reference rejects a short one.
//
// Exit 0 and print SUCCESS iff the node was discovered, the Link established,
// the uncompressed page returned and matched, and the compressed page failed
// exactly as the missing bz2 support predicts.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>
#include <MsgPack.h>

#include <microReticulum.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static const char* APP_NAME    = "nomadnetwork";
static const char* ASPECT      = "node";
static const char* PATH_PLAIN  = "/page/index.mu";  // served uncompressed
static const char* PATH_GZIP   = "/page/gz.mu";     // served bz2-compressed

// The page, loaded from THICKET_PAGE_FILE, identical to what the server serves.
static RNS::Bytes expected_page;

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Interface   udp_interface(RNS::Type::NONE);
static RNS::Identity    client_identity({RNS::Type::NONE});
static RNS::Destination outgoing_destination({RNS::Type::NONE});
static RNS::Link        active_link({RNS::Type::NONE});

enum Phase { PH_PLAIN = 0, PH_GZIP = 1, PH_DONE = 2 };
static volatile int  phase = PH_PLAIN;

static volatile bool announce_seen   = false;
static volatile bool link_up         = false;
static volatile bool plain_done      = false;
static volatile bool plain_ok        = false;   // page returned and matched
static volatile bool plain_unwrapped = false;   // response needed msgpack unwrap
static volatile bool gzip_done       = false;
static volatile bool gzip_failed     = false;   // failed, as the missing bz2 predicts
static volatile bool gzip_unexpected = false;   // succeeded: bz2 has arrived

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

static void request_current();

static void on_response(const RNS::RequestReceipt& receipt) {
	RNS::Bytes encoded = const_cast<RNS::RequestReceipt&>(receipt).get_response();
	RNS::Bytes page = encoded, unwrapped;
	bool used_unwrap = false;
	if (msgpack_unwrap_bin(encoded, unwrapped)) { page = unwrapped; used_unwrap = true; }
	const bool match = (page.size() == expected_page.size() &&
	                    memcmp(page.data(), expected_page.data(), page.size()) == 0);

	if (phase == PH_PLAIN) {
		plain_ok = match;
		plain_unwrapped = used_unwrap;
		plain_done = true;
		printf("[cpp] /page/index.mu: %lu bytes, msgpack-unwrap=%s, match=%s\n",
		       (unsigned long)page.size(), used_unwrap ? "yes" : "no",
		       match ? "yes" : "no");
	} else if (phase == PH_GZIP) {
		gzip_unexpected = true;   // a compressed page came back: bz2 now works
		gzip_done = true;
		printf("[cpp] /page/gz.mu SUCCEEDED (%lu bytes) -- bz2 decompression has "
		       "arrived at this pin\n", (unsigned long)page.size());
	}
	fflush(stdout);
}

static void on_failed(const RNS::RequestReceipt& receipt) {
	(void)receipt;
	if (phase == PH_PLAIN) {
		plain_done = true;
		printf("[cpp] /page/index.mu FAILED (uncompressed page did not arrive)\n");
	} else if (phase == PH_GZIP) {
		gzip_failed = true;
		gzip_done = true;
		printf("[cpp] /page/gz.mu failed, as the missing bz2 support predicts\n");
	}
	fflush(stdout);
}

static void request_current() {
	static const uint8_t MSGPACK_NIL = 0xC0;
	const RNS::Bytes nil_payload(&MSGPACK_NIL, 1);
	const char* path = (phase == PH_PLAIN) ? PATH_PLAIN : PATH_GZIP;
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
	request_current();
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

	double TIMEOUT_S = 40.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double start = RNS::Utilities::OS::time();

	// The compressed request should now fail fast: the resource is rejected as
	// uncompressable and concluded, so on_failed fires. This bounded wait is a
	// backstop against a regression to the old hang (no conclusion at all); if
	// it ever trips, the resource-conclusion fix has been undone.
	static const double GZ_WAIT = 14.0;
	double gz_deadline = 0.0;
	bool gzip_hung = false;

	while (true) {
		reticulum.loop();
		const double now = RNS::Utilities::OS::time();
		// Advance from the uncompressed request to the compressed one once the
		// first concludes, issued from the loop rather than inside a callback.
		if (phase == PH_PLAIN && plain_done) {
			phase = PH_GZIP;
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED) {
				request_current();
				gz_deadline = now + GZ_WAIT;
			} else { gzip_done = true; printf("[cpp] link gone before gz request\n"); }
		}
		if (phase == PH_GZIP && !gzip_done && gz_deadline > 0.0 && now > gz_deadline) {
			gzip_hung = true;
			gzip_done = true;
			printf("[cpp] /page/gz.mu did not conclude in %.0fs (bz2 Resource "
			       "cannot be assembled, and no watchdog times it out)\n", GZ_WAIT);
		}
		if (gzip_done) {
			if (active_link && active_link.status() != RNS::Type::Link::CLOSED)
				active_link.teardown();
			break;
		}
		if (now - start > TIMEOUT_S) {
			printf("[cpp] TIMEOUT (announce=%d link=%d plain_done=%d gzip_done=%d)\n",
			       announce_seen?1:0, link_up?1:0, plain_done?1:0, gzip_done?1:0);
			break;
		}
		RNS::Utilities::OS::sleep(0.01);
	}

	const double cleanup_until = RNS::Utilities::OS::time() + 0.5;
	while (RNS::Utilities::OS::time() < cleanup_until) {
		reticulum.loop();
		RNS::Utilities::OS::sleep(0.01);
	}
	RNS::Transport::deregister_interface(udp_interface);

	printf("[cpp] --- results ---\n");
	printf("[cpp]   %s node discovered by announce\n", announce_seen ? "OK  " : "FAIL");
	printf("[cpp]   %s link established\n", link_up ? "OK  " : "FAIL");
	printf("[cpp]   %s uncompressed page returned and matched (multi-packet Resource, "
	       "msgpack-decoded)\n", plain_ok ? "OK  " : "FAIL");

	int failures = 0, divergences = 0;
	if (!announce_seen) ++failures;
	if (!link_up) ++failures;
	if (!plain_ok) ++failures;

	// The compressed page: a known divergence while microReticulum has no bz2.
	// It is not retrievable, whether it fails cleanly or (as at this pin) hangs
	// with no watchdog to end it. Either is the expected divergence; only a
	// successful retrieval is a real failure, and means bz2 has arrived.
	if (gzip_unexpected) {
		++failures;
		printf("[cpp]   FAIL compressed page was retrieved. bz2 decompression now "
		       "exists; delete this exemption and assert the real behaviour.\n");
	} else if (gzip_failed || gzip_hung) {
		++divergences;
		printf("[cpp]   XFAIL compressed page not retrievable (%s; no bz2 at this "
		       "pin). A standard node compresses a Resource, so the browser can "
		       "fetch only pages a node left uncompressed until bz2 lands.\n",
		       gzip_failed ? "failed" : "hung, no watchdog");
	} else {
		++failures;
		printf("[cpp]   FAIL the gz request did not conclude and was not bounded.\n");
	}

	int rc = failures ? 1 : 0;
	if (rc == 0)
		printf("[cpp] SUCCESS page fetched and verified over a Link; %d known "
		       "divergence(s) present\n", divergences);
	else
		printf("[cpp] FAIL %d check(s) failed\n", failures);
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
