// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See LiveFetch.h. This is the page-fetch interop lifecycle, made synchronous.

#include "LiveFetch.h"

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>
#include <MsgPack.h>

#include <microReticulum.h>

#include <cctype>
#include <cstring>
#include <map>
#include <string>

namespace thicket {

namespace {

const char* APP_NAME = "nomadnetwork";
const char* ASPECT   = "node";

// The RNS stack is brought up once and reused across fetches: one identity, one
// interface, one path table. These match the interop requester's globals.
bool           g_ready = false;
RNS::Reticulum g_reticulum({RNS::Type::NONE});
RNS::Interface g_udp({RNS::Type::NONE});
RNS::Identity  g_identity({RNS::Type::NONE});
RNS::Link      g_link({RNS::Type::NONE});

// Node identities seen by announce, keyed by destination-hash hex. A page link
// carries the node's destination hash; the announce carries the public key we
// need to address it, exactly as the interop requester builds its destination
// from the announced identity rather than assuming one.
std::map<std::string, RNS::Identity> g_known;

// Per-fetch state, reset at the top of every call. Single-threaded: one page is
// fetched at a time, so file-static state is safe and mirrors the interop side.
volatile bool g_link_up   = false;
volatile bool g_done      = false;
volatile bool g_response  = false;
RNS::Bytes    g_page;
std::string   g_path;

std::string to_lower_hex(const std::string& s) {
	std::string out = s;
	for (char& c : out) c = (char)std::tolower((unsigned char)c);
	return out;
}

// Unwrap a msgpack `bin` value. A page served as bytes arrives msgpack-wrapped,
// so the Resource payload is decoded the same way the interop side decodes it.
bool msgpack_unwrap_bin(const RNS::Bytes& packed, RNS::Bytes& out) {
	if (packed.size() < 1) return false;
	MsgPack::Unpacker u;
	u.feed(packed.data(), packed.size());
	if (!u.isBin()) return false;
	MsgPack::bin_t<uint8_t> bin;
	u.deserialize(bin);
	out = RNS::Bytes(bin.data(), bin.size());
	return true;
}

void on_response(const RNS::RequestReceipt& receipt) {
	RNS::Bytes encoded = const_cast<RNS::RequestReceipt&>(receipt).get_response();
	RNS::Bytes unwrapped;
	if (msgpack_unwrap_bin(encoded, unwrapped)) g_page = unwrapped;
	else                                        g_page = encoded;
	g_response = true;
	g_done = true;
}

void on_failed(const RNS::RequestReceipt& receipt) {
	(void)receipt;
	g_response = false;
	g_done = true;
}

void on_link_closed(RNS::Link& link) {
	(void)link;
	// A close before the response concludes ends the fetch; the watchdog uses
	// this path to fail a silent peer instead of hanging.
	if (!g_response) g_done = true;
}

void on_link_established(RNS::Link& link) {
	g_link_up = true;
	link.identify(g_identity);
	static const uint8_t MSGPACK_NIL = 0xC0;  // a page fetch carries no body
	const RNS::Bytes nil_payload(&MSGPACK_NIL, 1);
	link.request(RNS::Bytes(g_path), nil_payload,
	             on_response, on_failed, nullptr, /*timeout=*/10.0);
}

class NodeAnnounceHandler : public RNS::AnnounceHandler {
public:
	NodeAnnounceHandler() : RNS::AnnounceHandler(
	    (std::string(APP_NAME) + "." + ASPECT).c_str()) {}
	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		(void)app_data;
		g_known[to_lower_hex(destination_hash.toHex())] = announced_identity;
	}
};
RNS::HAnnounceHandler g_announce_handler(new NodeAnnounceHandler());

// Bring the stack up once. The UDP endpoint is compile-time (DEFAULT_UDP_*),
// the same knob the interop scenarios set, so a build points at whichever rnsd
// bridges to the mesh.
bool ensure_ready() {
	if (g_ready) return true;

	static microStore::FileSystem filesystem{
	    microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	g_udp = new UDPInterface();
	g_udp.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(g_udp);
	g_udp.start();

	g_reticulum = RNS::Reticulum();
	g_reticulum.transport_enabled(false);
	g_reticulum.start();

	g_identity = RNS::Identity();
	RNS::Transport::register_announce_handler(g_announce_handler);

	g_ready = true;
	return true;
}

// Split "<hash>:/page/<path>" into a lowercased hex hash and the page path.
bool parse_target(const std::string& target, std::string& hash, std::string& path) {
	auto c = target.find(':');
	if (c == std::string::npos) return false;
	std::string h = target.substr(0, c);
	if (h.size() < 8) return false;
	for (char ch : h)
		if (!std::isxdigit((unsigned char)ch)) return false;
	hash = to_lower_hex(h);
	path = target.substr(c + 1);
	return !path.empty() && path[0] == '/';
}

}  // namespace

FetchResult live_fetch(const std::string& target, double timeout_s) {
	FetchResult r;

	std::string hash_hex;
	if (!parse_target(target, hash_hex, g_path)) {
		r.error = "link has no mesh address to fetch: " + target;
		return r;
	}

	if (!ensure_ready()) {
		r.error = "could not bring up the network stack";
		return r;
	}

	// Reset per-fetch state.
	g_link_up = false;
	g_done = false;
	g_response = false;
	g_page = RNS::Bytes();

	const double start = RNS::Utilities::OS::time();
	RNS::Bytes dh;
	dh.assignHex(hash_hex.c_str());

	// Discovery: ask the network for a path to the node, then wait for the
	// announce that carries its identity. The interop side discovers by announce
	// too; here a path request nudges a node that is not currently announcing.
	RNS::Transport::request_path(dh);
	const double discover_deadline = start + timeout_s * 0.5;
	RNS::Identity node_identity({RNS::Type::NONE});
	while (RNS::Utilities::OS::time() < discover_deadline) {
		g_reticulum.loop();
		auto it = g_known.find(hash_hex);
		if (it != g_known.end() && (bool)it->second) { node_identity = it->second; break; }
		RNS::Identity recalled = RNS::Identity::recall(dh);
		if ((bool)recalled) { node_identity = recalled; break; }
		RNS::Utilities::OS::sleep(0.01);
	}
	if (!(bool)node_identity) {
		r.error = "no route to " + hash_hex + " (no announce or path within "
		          + std::to_string((int)(timeout_s * 0.5)) + "s)";
		return r;
	}

	// Establish the Link and request the page. The established callback fires
	// request(); response/failed callbacks conclude it. The watchdog bounds a
	// silent peer.
	RNS::Destination dest(node_identity,
	                      RNS::Type::Destination::OUT,
	                      RNS::Type::Destination::SINGLE,
	                      APP_NAME, ASPECT);
	g_link = RNS::Link(dest, on_link_established, on_link_closed);

	const double deadline = start + timeout_s;
	while (!g_done && RNS::Utilities::OS::time() < deadline) {
		g_reticulum.loop();
		if (g_link && g_link.status() == RNS::Type::Link::CLOSED && !g_response) break;
		RNS::Utilities::OS::sleep(0.01);
	}

	if (g_link && g_link.status() != RNS::Type::Link::CLOSED) g_link.teardown();
	// Let the teardown leave the wire.
	const double drain = RNS::Utilities::OS::time() + 0.3;
	while (RNS::Utilities::OS::time() < drain) {
		g_reticulum.loop();
		RNS::Utilities::OS::sleep(0.01);
	}

	if (g_response && g_page.size() > 0) {
		r.ok = true;
		r.page.assign((const char*)g_page.data(), g_page.size());
		return r;
	}
	if (!g_link_up)       r.error = "link to " + hash_hex + " never established";
	else if (!g_done)     r.error = "page request timed out";
	else                  r.error = "page request failed (peer refused, gone, or over the size cap)";
	return r;
}

}  // namespace thicket
