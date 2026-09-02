#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Originator for the TRANSPORT FORWARDING scenario, the one where our C++ stack
is the router rather than the leaf.

    this process            C++ router            forward_endpoint.py
    14280 <-----------> 14281 | 14283 <-----------> 14282
             segment A                  segment B

The two Python peers share no interface and cannot reach each other directly.
So a payload arriving at the far end is proof the C++ node forwarded it, and
the hop count is proof of how.

This process spawns the far end as a child, which keeps the interop driver's
one-Python-one-C++ contract while still putting two independent RNS instances
on opposite sides of the node under test. Its own exit code folds in the
child's: the far end holds the assertions that matter, because a router cannot
credibly certify its own delivery.

Exit codes:
  0  the far end received the payload at the expected hop count
  1  it received the wrong thing, or by the wrong path
  2  timeout
  3  setup error
"""

import argparse
import atexit
import hashlib
import os
import signal
import subprocess
import sys
import tempfile
import time

try:
    import RNS
except ImportError as e:                                  # pragma: no cover
    print(f"[python] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

APP_NAME = "thicket_interop"
ASPECT = "forward"
PAYLOAD_LEN = 383


def build_payload(n=PAYLOAD_LEN):
    out = bytearray()
    x = 0x5EED1E5F  # same generator as the other scenarios
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append((x >> 24) & 0xFF)
    return bytes(out)


def write_config(config_dir, port, forward_port):
    cfg = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37478
  instance_control_port = 37479
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  [[UDPOrig]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {port}
    forward_ip = 127.0.0.1
    forward_port = {forward_port}
"""
    with open(os.path.join(config_dir, "config"), "w") as f:
        f.write(cfg)


def _kill(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=14280)
    ap.add_argument("--forward-port", type=int, default=14281)
    ap.add_argument("--far-port", type=int, default=14282)
    ap.add_argument("--far-forward-port", type=int, default=14283)
    ap.add_argument("--timeout", type=float, default=60.0)
    # A packet leaves here at hops=0. The router counts one on ingress, the far
    # end counts another. Two is therefore the signature of having crossed
    # exactly one forwarding node.
    ap.add_argument("--expect-hops", type=int, default=2)
    #   nohop  -- tell the far end to expect a hop count that would mean the
    #             packet never crossed the router, proving the check is live.
    ap.add_argument("--self-test-break", choices=("none", "nohop"),
                    default="none")
    args = ap.parse_args()

    idfile = os.path.join(tempfile.mkdtemp(prefix="thicket_fwd_id_"), "far_id")

    expect = args.expect_hops
    if args.self_test_break == "nohop":
        expect = 1
        print("[python] SELF-TEST BREAK: far end will expect hops=1, the count "
              "for a packet that never crossed the router", flush=True)

    far_cmd = [
        sys.executable,
        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "forward_endpoint.py"),
        "--port", str(args.far_port),
        "--forward-port", str(args.far_forward_port),
        "--timeout", str(args.timeout - 5),
        "--expect-hops", str(expect),
        "--identity-file", idfile,
    ]
    print(f"[python] spawning far end: {' '.join(far_cmd)}", flush=True)
    far = subprocess.Popen(far_cmd)
    atexit.register(_kill, far)

    def _sig(signum, _frame):
        _kill(far)
        sys.exit(128 + signum)
    for s in (signal.SIGTERM, signal.SIGINT):
        signal.signal(s, _sig)

    # Wait for the far end to write its identity. Addressing it from a file
    # rather than from an announce keeps this scenario about the DATA path: a
    # failure then cannot be confused with a lost announce.
    deadline = time.time() + 20
    while time.time() < deadline and not os.path.exists(idfile):
        if far.poll() is not None:
            print(f"[python] FAIL far end exited early with {far.returncode}",
                  file=sys.stderr, flush=True)
            sys.exit(3)
        time.sleep(0.1)
    if not os.path.exists(idfile):
        print("[python] FAIL far end never wrote its identity", file=sys.stderr)
        sys.exit(3)

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_orig_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.port, args.forward_port)
    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[python] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    far_identity = RNS.Identity.from_file(idfile)
    if far_identity is None:
        print("[python] FAIL could not load the far end's identity",
              file=sys.stderr)
        sys.exit(3)
    far_dest = RNS.Destination(far_identity, RNS.Destination.OUT,
                               RNS.Destination.SINGLE, APP_NAME, ASPECT)
    print(f"[python] far destination {far_dest.hash.hex()}", flush=True)

    payload = build_payload()
    print(f"[python] payload {len(payload)} bytes "
          f"sha256={hashlib.sha256(payload).hexdigest()}", flush=True)

    # Give the router time to hear the far end's announce and build a path.
    # Without a path it cannot forward, and the failure would look like a
    # forwarding bug rather than a timing one.
    sent = False
    start = time.time()
    while time.time() - start < args.timeout:
        if far.poll() is not None:
            break
        if not sent and RNS.Transport.has_path(far_dest.hash):
            RNS.Packet(far_dest, payload).send()
            sent = True
            print(f"[python] path known after {time.time()-start:.1f}s; "
                  f"originated {len(payload)} bytes", flush=True)
        elif not sent and time.time() - start > 20:
            # Send anyway and let the far end's silence be the verdict; a
            # never-sent packet would report as a timeout and hide the cause.
            RNS.Packet(far_dest, payload).send()
            sent = True
            print("[python] no path after 20s, sending regardless so the "
                  "failure is about forwarding, not about waiting", flush=True)
        time.sleep(0.1)

    rc = far.wait(timeout=10) if far.poll() is None else far.returncode
    print(f"[python] far end exited {rc}", flush=True)
    if rc == 0:
        print("[python] SUCCESS the C++ node forwarded between two peers that "
              "share no interface", flush=True)
        sys.exit(0)
    print("[python] FAILURE see the far end's output above", flush=True)
    sys.exit(rc if rc in (1, 2, 3) else 1)


if __name__ == "__main__":
    main()
