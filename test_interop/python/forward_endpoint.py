#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The far end of the TRANSPORT FORWARDING scenario.

It sits on the other side of our C++ router from the originator, on a UDP
segment the originator is not a member of. It announces, waits, and reports —
verbatim, on stdout — what it receives and at what hop count.

It is a separate process because the two Python peers must not share an
interface: if they did, they could reach each other directly and the scenario
would pass without our router doing anything. The originator spawns this one,
so the interop driver still sees one Python process and one C++ process.

Exit codes:
  0  received the expected payload at the expected hop count
  1  received something wrong, or the hop count says it did not cross the router
  2  received nothing
  3  setup error
"""

import argparse
import hashlib
import os
import sys
import tempfile
import time

try:
    import RNS
except ImportError as e:                                  # pragma: no cover
    print(f"[far] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

APP_NAME = "thicket_interop"
ASPECT = "forward"

state = {"got": None, "hops": None}


def write_config(config_dir, port, forward_port):
    # enable_transport = No: this is an endpoint, not a second router. If it
    # routed too, a delivery would not prove our node did anything.
    cfg = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37488
  instance_control_port = 37489
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  [[UDPFar]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {port}
    forward_ip = 127.0.0.1
    forward_port = {forward_port}
"""
    with open(os.path.join(config_dir, "config"), "w") as f:
        f.write(cfg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--forward-port", type=int, required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--expect-hops", type=int, default=2)
    ap.add_argument("--identity-file", required=True,
                    help="written so the originator can address us without "
                         "waiting to learn an announce through the router")
    args = ap.parse_args()

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_far_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.port, args.forward_port)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[far] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    identity = RNS.Identity()
    dest = RNS.Destination(identity, RNS.Destination.IN,
                           RNS.Destination.SINGLE, APP_NAME, ASPECT)

    def on_packet(data, packet):
        state["got"] = data
        state["hops"] = packet.hops
        print(f"[far] received {len(data)} bytes at hops={packet.hops}",
              flush=True)

    dest.set_packet_callback(on_packet)
    dest.set_proof_strategy(RNS.Destination.PROVE_ALL)

    # Hand our identity to the originator on disk rather than over the air.
    # Learning it from an announce would work, but then a failure could not be
    # told apart from "the announce did not arrive", and this scenario is about
    # the DATA path.
    identity.to_file(args.identity_file)
    print(f"[far] identity written to {args.identity_file}", flush=True)
    print(f"[far] destination {dest.hash.hex()}", flush=True)

    # Announce anyway: the router needs a path to us, and this is how it gets
    # one. It is also what the C++ side counts.
    dest.announce()
    print("[far] announced", flush=True)

    start = time.time()
    last_announce = time.time()
    while time.time() - start < args.timeout:
        if state["got"] is not None:
            break
        # Re-announce: the router may not have been listening yet on the first.
        if time.time() - last_announce >= 5.0:
            dest.announce()
            last_announce = time.time()
        time.sleep(0.05)

    if state["got"] is None:
        print("[far] TIMEOUT nothing arrived. Either the router did not "
              "forward, or it never had a path to us.", flush=True)
        sys.exit(2)

    digest = hashlib.sha256(state["got"]).hexdigest()
    print(f"[far] payload sha256={digest}", flush=True)

    if state["hops"] != args.expect_hops:
        print(f"[far] FAILURE hops={state['hops']}, expected "
              f"{args.expect_hops}. RNS counts a hop on ingress at every node, "
              f"so a packet that really crossed the router is counted twice — "
              f"once by it, once by us. A lower number means it reached us by "
              f"some path that did not go through the node under test, and the "
              f"scenario proved nothing.", flush=True)
        sys.exit(1)

    print(f"[far] SUCCESS payload arrived across the router at hops="
          f"{state['hops']}", flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
