#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Transport relay for the MULTI-HOP INBOUND interop scenario.

This process is deliberately dumb. It is a stock Python RNS node with
`enable_transport = Yes` and two point-to-point UDP interfaces, one facing the
C++ leaf and one facing the Python originator. It never announces, never
originates, and holds no destination of its own -- it exists only to force
traffic between the other two to be *routed* rather than delivered directly.

Why a separate process at all: the interop driver launches exactly one Python
process and one C++ process per scenario, so the originator spawns this as a
child to keep that contract intact (see run_multihop_inbound.sh).

Why the reference implementation and not our own: the point of the scenario is
to test OUR leaf against the reference's routing, so the routing has to be the
reference's. A relay built from the C++ port would be testing our transport
against itself.
"""

import argparse
import os
import sys
import tempfile
import time

try:
    import RNS
except ImportError as e:                                  # pragma: no cover
    print(f"[relay] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)


def write_config(config_dir: str, leaf_port: int, leaf_forward: int,
                 origin_port: int, origin_forward: int) -> None:
    # enable_transport = Yes is the entire reason this node exists. Without it
    # RNS accepts packets on both interfaces and forwards between neither, and
    # the scenario would time out rather than fail loudly -- which is why the
    # originator asserts on the hop count and not merely on delivery.
    cfg = f"""
[reticulum]
  enable_transport = Yes
  share_instance = No
  shared_instance_port = 37478
  instance_control_port = 37479
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  [[UDPToLeaf]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {leaf_port}
    forward_ip = 127.0.0.1
    forward_port = {leaf_forward}

  [[UDPToOrigin]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {origin_port}
    forward_ip = 127.0.0.1
    forward_port = {origin_forward}
"""
    with open(os.path.join(config_dir, "config"), "w") as f:
        f.write(cfg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leaf-port", type=int, required=True)
    ap.add_argument("--leaf-forward-port", type=int, required=True)
    ap.add_argument("--origin-port", type=int, required=True)
    ap.add_argument("--origin-forward-port", type=int, required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_relay_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.leaf_port, args.leaf_forward_port,
                 args.origin_port, args.origin_forward_port)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[relay] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    print(f"[relay] transport node up: leaf {args.leaf_port}->"
          f"{args.leaf_forward_port}, origin {args.origin_port}->"
          f"{args.origin_forward_port}", flush=True)
    print(f"[relay] transport_enabled="
          f"{RNS.Reticulum.transport_enabled()}", flush=True)

    # Nothing to do but stay alive and route. The parent kills us.
    start = time.time()
    while time.time() - start < args.timeout:
        time.sleep(0.2)
    print("[relay] timeout reached, exiting", flush=True)


if __name__ == "__main__":
    main()
