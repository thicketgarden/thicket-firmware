#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python side of the NomadNet page-fetch interop scenario.

A stock RNS node on the NomadNet aspect ("nomadnetwork.node"). It serves a real
page (desktop/pages/index.mu, passed with --page-file) at four paths:

  /page/index.mu   auto_compress=False -- an uncompressed Resource.
  /page/gz.mu      auto_compress=True -- the same page, bz2-compressed by RNS.
                   The C++ side decompresses it on-device (capped bz2) and
                   verifies it matches.
  /page/mid.mu     auto_compress=True -- a 16 KB deterministic page that
                   compresses to ~8 KB, so it arrives as a multi-packet
                   compressed Resource under the cap. The larger-page case.
  /page/big.mu     auto_compress=True -- a page over the on-device size cap,
                   which the C++ side must reject cleanly rather than hang.

This is what a real NomadNet node does; the C++ side is the browser's fetch edge.

Exit 0 and print SUCCESS iff all four handlers were hit. Exit 2 on timeout, 3
on setup error.
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
PATH_PLAIN = "/page/index.mu"
PATH_GZIP = "/page/gz.mu"
PATH_MID = "/page/mid.mu"   # compressed, multi-packet, under the device cap
PATH_BIG = "/page/big.mu"   # compressed, larger than the device cap
NODE_NAME = "Interop Test Node"

state = {"plain": False, "gzip": False, "mid": False, "big": False,
         "page": b"", "midpage": b"", "bigpage": b""}


def serve_plain(path, data, request_id, link_id, remote_identity, requested_at):
    print(f"[python] handler {PATH_PLAIN} called (uncompressed)", flush=True)
    state["plain"] = True
    return state["page"]


def serve_gzip(path, data, request_id, link_id, remote_identity, requested_at):
    print(f"[python] handler {PATH_GZIP} called (auto_compress)", flush=True)
    state["gzip"] = True
    return state["page"]


def serve_mid(path, data, request_id, link_id, remote_identity, requested_at):
    print(f"[python] handler {PATH_MID} called ({len(state['midpage'])}B, auto_compress)", flush=True)
    state["mid"] = True
    return state["midpage"]


def serve_big(path, data, request_id, link_id, remote_identity, requested_at):
    print(f"[python] handler {PATH_BIG} called ({len(state['bigpage'])}B, auto_compress)", flush=True)
    state["big"] = True
    return state["bigpage"]


def gen_page(n, seed=0x1234):
    # Same LCG the C++ side runs, so the bytes match exactly. A 16-symbol
    # alphabet compresses to about half, so this arrives as a multi-packet
    # compressed Resource yet decompresses under the cap.
    x = seed
    A = b"0123456789ABCDEF"
    out = bytearray()
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out.append(A[(x >> 16) & 0xF])
    return bytes(out)


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
    ap.add_argument("--page-file", required=True)
    ap.add_argument("--timeout", type=float, default=40.0)
    args = ap.parse_args()

    try:
        with open(args.page_file, "rb") as f:
            state["page"] = f.read()
    except OSError as e:
        print(f"[python] cannot read page file: {e}", file=sys.stderr)
        sys.exit(3)
    # A page comfortably over the 24KB device cap, compressible so RNS sends it
    # as a bz2 Resource the device must reject (over cap) rather than hang.
    state["midpage"] = gen_page(16384)   # ~16 KB -> ~8 KB compressed, multi-packet, under cap
    state["bigpage"] = (b">Big Interop Page\n\nA line of page content that repeats.\n") * 900
    print(f"[python] serving {args.page_file}: {len(state['page'])} bytes; "
          f"mid page {len(state['midpage'])} bytes; big page {len(state['bigpage'])} bytes",
          flush=True)

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
    destination.register_request_handler(PATH_PLAIN, serve_plain,
                                         allow=RNS.Destination.ALLOW_ALL,
                                         auto_compress=False)
    destination.register_request_handler(PATH_GZIP, serve_gzip,
                                         allow=RNS.Destination.ALLOW_ALL,
                                         auto_compress=True)
    destination.register_request_handler(PATH_MID, serve_mid,
                                         allow=RNS.Destination.ALLOW_ALL,
                                         auto_compress=True)
    destination.register_request_handler(PATH_BIG, serve_big,
                                         allow=RNS.Destination.ALLOW_ALL,
                                         auto_compress=True)

    print(f"[python] node hash: {destination.hash.hex()}", flush=True)
    destination.announce(app_data=NODE_NAME.encode("utf-8"))
    print("[python] announced", flush=True)

    start = time.time()
    last_announce = start
    while time.time() - start < args.timeout:
        if not state["plain"] and time.time() - last_announce >= 2.0:
            destination.announce(app_data=NODE_NAME.encode("utf-8"))
            last_announce = time.time()
        if state["plain"] and state["gzip"] and state["mid"] and state["big"]:
            time.sleep(1.0)  # let the last response leave the wire
            print("[python] SUCCESS served the plain, compressed, mid, and big page",
                  flush=True)
            sys.exit(0)
        time.sleep(0.05)

    print(f"[python] TIMEOUT (plain={state['plain']} gzip={state['gzip']} mid={state['mid']} big={state['big']})",
          flush=True)
    sys.exit(2)


if __name__ == "__main__":
    main()
