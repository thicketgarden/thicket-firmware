// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live page fetch for the desktop browser.
//
// The browser resolves a Micron link to a local file first. When no local page
// is held, the target names a node on the mesh, and this fetches it: it brings
// up microReticulum over the UDP bridge to a local rnsd, discovers the node,
// establishes a Link, requests the page path, and returns the decoded Micron.
//
// It is the same Link + Resource + msgpack + request(path) lifecycle proven in
// the page-fetch interop scenario, reduced to one synchronous call the browser
// makes on its "not held locally" edge. The renderer, parser and framebuffer
// are untouched: this hands raw Micron bytes to the same compose_page().
//
// Synchronous by design. The browser's navigation is one page at a time, so a
// fetch blocks until it returns a page, fails, or times out. A silent or dead
// peer is bounded by the Link watchdog and the request timeout, so this cannot
// hang; a compressed page is decompressed on the way in, capped like the
// device.

#pragma once

#include <string>

namespace thicket {

struct FetchResult {
	bool ok = false;
	std::string page;    // decoded Micron bytes, valid when ok
	std::string error;   // human-readable reason, valid when !ok
};

// Fetch the page named by a Micron link target of the form
// "<destination-hash>:/page/<path>", over the UDP bridge to a local rnsd.
// timeout_s bounds the whole round trip (discovery, link, request).
FetchResult live_fetch(const std::string& target, double timeout_s = 30.0);

}  // namespace thicket
