#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python side of the NomadNet page-fetch interop scenario.

A stock RNS node on the NomadNet aspect ("nomadnetwork.node"). It registers a
request handler at "/page/index.mu" that serves a known Micron page, announces,
and waits for the C++ requester to establish a Link and fetch it. This is what a
real NomadNet node does; the C++ side is the browser's fetch edge.

Exit 0 and print SUCCESS iff the handler served the page. Exit 2 on timeout,
3 on setup error.
"""

import argparse
import os
import sys
import tempfile
import time

try:
    import RNS
except ImportError as e:  # pragma: no cover
    print(f"[python] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

APP_NAME = "nomadnetwork"
ASPECT = "node"
PAGE_PATH = "/page/index.mu"
NODE_NAME = "Interop Test Node"

# The known page, byte-identical to EXPECTED_PAGE in the C++ requester. Kept
# small so the response is a single-packet RPC and never a compressed Resource.
PAGE = (
    b">Interop Test Node\n"
    b"\n"
    b"This page proves the C++ page-fetch lifecycle end to end.\n"
    b"\n"
    b"`[Home`:/page/index.mu]\n"
)

state = {"served": False}


def serve_page(path, data, request_id, link_id, remote_identity, requested_at):
    print(f"[python] request handler called: path={path!r}", flush=True)
    state["served"] = True
    return PAGE


def on_link_established(link):
    print(f"[python] link established: {link.link_id.hex()}", flush=True)


def write_config(config_dir, listen_port, forward_port):
    cfg = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37448
  instance_control_port = 37449
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  [[UDPInterop]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {listen_port}
    forward_ip = 127.0.0.1
    forward_port = {forward_port}
"""
    with open(os.path.join(config_dir, "config"), "w") as f:
        f.write(cfg)


def main():
    ap = argparse.ArgumentParser()
    # C++ owns LOCAL 14300 / REMOTE 14301, so the node listens on 14301 and
    # forwards to 14300.
    ap.add_argument("--listen-port", type=int, default=14301)
    ap.add_argument("--forward-port", type=int, default=14300)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    config_dir = tempfile.mkdtemp(prefix="rns_interop_page_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.listen_port, args.forward_port)
    print(f"[python] config dir: {config_dir}", flush=True)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:  # pragma: no cover
        print(f"[python] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    identity = RNS.Identity()
    destination = RNS.Destination(identity, RNS.Destination.IN,
                                  RNS.Destination.SINGLE, APP_NAME, ASPECT)
    destination.set_link_established_callback(on_link_established)
    destination.set_proof_strategy(RNS.Destination.PROVE_ALL)
    # ALLOW_ALL, as a public node serving a page to anyone who can reach it.
    destination.register_request_handler(PAGE_PATH, serve_page,
                                         allow=RNS.Destination.ALLOW_ALL)

    print(f"[python] node hash: {destination.hash.hex()}", flush=True)
    destination.announce(app_data=NODE_NAME.encode("utf-8"))
    print("[python] announced", flush=True)

    start = time.time()
    last_announce = start
    while time.time() - start < args.timeout:
        if not state["served"] and time.time() - last_announce >= 2.0:
            destination.announce(app_data=NODE_NAME.encode("utf-8"))
            last_announce = time.time()
        if state["served"]:
            # Give the response a moment to leave the wire before we exit.
            time.sleep(1.0)
            print("[python] SUCCESS served the page over a Link", flush=True)
            sys.exit(0)
        time.sleep(0.05)

    print("[python] TIMEOUT the page was never requested", flush=True)
    sys.exit(2)


if __name__ == "__main__":
    main()
