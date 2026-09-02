#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python originator for the COLD INBOUND and MULTI-HOP INBOUND interop scenarios.

One script serves both because the difference between them is topology, not
behaviour: with --relay it spawns a transport-enabled RNS node (multihop_relay)
and binds the socket one segment further out, so the C++ leaf's own config is
untouched and it can't tell it's no longer being addressed directly. The leaf
then has to handle a path learned through a relay and a packet that arrived
with a non-zero hop count. See run_multihop_inbound.sh for the topology
diagram.

This is the half of the scenario that makes it cold. It:

  1. Brings up RNS on a loopback UDP interface.
  2. **Never announces.** The C++ peer therefore can't learn this side's
     identity and can't have transmitted to it. That's the whole point:
     every existing microReticulum scenario has the C++ side speak first.
  3. Learns the C++ destination from the C++ side's announce.
  4. Originates a single encrypted DATA packet to it, at exactly
     ENCRYPTED_MDU bytes.
  5. Requires TWO independent confirmations before it will claim success:

     a. RNS validates the C++ side's proof for the packet. This exercises the
        C++ signing path against the reference -- but note it does NOT prove
        decryption: both implementations emit a proof from Transport after
        handing the packet to the destination, decrypted or not.
     b. A later C++ announce carries app_data equal to
        sha256(payload || b"thicket-cold-ack")[:16] || hops. Only a peer that
        actually recovered the plaintext can produce the digest, so that is
        the assertion that the cold decrypt really happened; the trailing hop
        byte is what pins the topology, so a run that was supposed to be
        relayed can't pass by arriving directly.

Exit codes:
  0  packet sent AND proof validated AND plaintext digest confirmed
  1  proof failed, or the digest never appeared / didn't match
  2  timeout waiting for the C++ announce
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


def _kill_relay(proc):
    """Never leave a transport node bound to a port a later run will want."""
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def _install_relay_reaper(proc):
    """atexit alone isn't enough here.

    The driver ends every scenario by SIGTERM-ing this process, and Python's
    default SIGTERM disposition exits without running atexit handlers. That
    would orphan the relay still bound to the loopback ports, and the next
    scenario in run_all.sh would fail for a reason that has nothing to do with
    what it's testing. Convert the signal into a normal exit so the handler
    registered above actually runs.
    """
    atexit.register(_kill_relay, proc)

    def _handler(signum, _frame):
        _kill_relay(proc)
        sys.exit(128 + signum)

    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, _handler)

try:
    import RNS
except ImportError as e:                                  # pragma: no cover
    print(f"[python] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

APP_NAME = "thicket_interop"
ASPECT = "cold"

PAYLOAD_LEN = 383  # RNS 1.4.2 ENCRYPTED_MDU; asserted below.


def build_payload(n: int = PAYLOAD_LEN) -> bytes:
    """Must match build_expected_payload() in the C++ receiver, byte for byte."""
    out = bytearray()
    x = 0x5EED1E5F
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append((x >> 24) & 0xFF)
    return bytes(out)


ACK_TAG = b"thicket-cold-ack"


def expected_ack(payload: bytes, hops: int) -> bytes:
    """Digest of the plaintext, then the hop count the C++ side observed.

    The hop byte is what makes this scenario's topology assertion real. RNS
    increments hops on ingress, so a directly-delivered packet reports 1 and
    each transport node in the path adds one more. Pinning the exact value
    means a relayed run that somehow arrived direct fails rather than passes.
    """
    return hashlib.sha256(payload + ACK_TAG).digest()[:16] + bytes([hops])


state = {
    "remote_identity": None,
    "remote_dest_hash": None,
    "sent": False,
    "proven": False,
    "ack_ok": False,
    "failed": False,
    "expected_ack": b"",
    "app_data_seen": set(),
    "ignore_dest": None,
    "relay_proc": None,
    "hops_wrong": None,
}


class _AnnounceHandler:
    def __init__(self):
        self.aspect_filter = f"{APP_NAME}.{ASPECT}"

    def received_announce(self, destination_hash, announced_identity, app_data):
        # Only relevant under --self-test-break coldness, where this side
        # announces on purpose and would otherwise address itself.
        if destination_hash == state["ignore_dest"]:
            return
        if state["remote_identity"] is None:
            state["remote_identity"] = announced_identity
            state["remote_dest_hash"] = destination_hash
            print(f"[python] learned C++ destination from announce: "
                  f"{destination_hash.hex()}", flush=True)
        if destination_hash != state["remote_dest_hash"]:
            return
        if not app_data:
            return
        if app_data in state["app_data_seen"]:
            return
        state["app_data_seen"].add(app_data)
        match = (app_data == state["expected_ack"])
        print(f"[python] announce app_data={app_data.hex()} "
              f"expected={state['expected_ack'].hex()} match={match}",
              flush=True)
        # Separate the two failure modes, because they mean opposite things:
        # a wrong digest is a crypto/parity bug, a wrong hop count means the
        # packet didn't travel the path this scenario claims to be testing.
        if not match and len(app_data) == len(state["expected_ack"]) == 17:
            if app_data[:16] == state["expected_ack"][:16]:
                state["hops_wrong"] = (app_data[16], state["expected_ack"][16])
                print(f"[python] plaintext digest is CORRECT but hop count is "
                      f"{app_data[16]}, expected {state['expected_ack'][16]} "
                      f"-- the packet did not take the expected path",
                      flush=True)
            elif app_data[16] == state["expected_ack"][16]:
                print("[python] hop count is correct but the plaintext digest "
                      "does not match -- the peer recovered different bytes",
                      flush=True)
        if match:
            state["ack_ok"] = True


def on_delivered(receipt):
    state["proven"] = True
    print("[python] proof received and VALIDATED by RNS "
          f"(receipt status={receipt.status})", flush=True)


def on_timeout(receipt):
    state["failed"] = True
    print(f"[python] packet receipt TIMED OUT (status={receipt.status}); "
          "the C++ side did not return a valid proof", flush=True)


def write_config(config_dir: str, port: int, forward_port: int) -> None:
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
    listen_port = {port}
    forward_ip = 127.0.0.1
    forward_port = {forward_port}
"""
    with open(os.path.join(config_dir, "config"), "w") as f:
        f.write(cfg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=14262)
    ap.add_argument("--forward-port", type=int, default=14263)
    ap.add_argument("--timeout", type=float, default=40.0)
    # MULTI-HOP mode. The C++ leaf's own config is untouched: the relay
    # takes over the socket this script would otherwise have bound, so the leaf
    # can't tell it's no longer talking to the originator directly -- which
    # is the property being tested.
    ap.add_argument("--relay", action="store_true",
                    help="insert a transport-enabled Python relay between this "
                         "script and the C++ leaf")
    ap.add_argument("--origin-port", type=int, default=14264)
    ap.add_argument("--origin-forward-port", type=int, default=14265)
    ap.add_argument("--expect-hops", type=int, default=None,
                    help="hop count the C++ side must report (default: 1 "
                         "direct, 2 relayed)")
    # A test that has never been seen to fail isn't evidence. These
    # switches make the failure re-runnable by anyone, at any time, instead of
    # living in a report someone has to trust. They're opt-in and default off.
    #   payload   -- flip one byte of the payload on the wire; the C++ side
    #                must notice and refuse to ack.
    #   coldness  -- announce from this side, which is exactly the condition
    #                that would silently turn this back into the already-covered
    #                warm case; the C++ coldness guard must catch it.
    #   hops      -- expect the hop count for the OTHER topology, proving the
    #                hop byte is genuinely compared rather than carried along.
    ap.add_argument("--self-test-break",
                    choices=("none", "payload", "coldness", "hops"),
                    default="none",
                    help="deliberately break one assertion, to prove it is live")
    args = ap.parse_args()

    if RNS.Packet.ENCRYPTED_MDU != PAYLOAD_LEN:
        # Not fatal, but the scenario's claim to be testing the maximum
        # single-packet payload is only true if these agree. Say so loudly
        # rather than quietly testing something smaller than advertised.
        print(f"[python] NOTE: RNS.Packet.ENCRYPTED_MDU is "
              f"{RNS.Packet.ENCRYPTED_MDU}, scenario pins {PAYLOAD_LEN}",
              flush=True)

    # Start the relay BEFORE this side's Reticulum comes up, so the leaf-facing
    # socket is already bound when the C++ leaf starts announcing into it.
    if args.relay:
        my_port, my_forward = args.origin_forward_port, args.origin_port
        relay_cmd = [
            sys.executable,
            os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "multihop_relay.py"),
            "--leaf-port", str(args.port),
            "--leaf-forward-port", str(args.forward_port),
            "--origin-port", str(args.origin_port),
            "--origin-forward-port", str(args.origin_forward_port),
            "--timeout", str(args.timeout + 10),
        ]
        print(f"[python] spawning relay: {' '.join(relay_cmd)}", flush=True)
        relay_proc = subprocess.Popen(relay_cmd)
        # The relay must outlive nothing and precede everything; if it dies the
        # scenario must not quietly degrade into the direct case, so its exit is
        # checked in the loop below rather than only at the end.
        _install_relay_reaper(relay_proc)
        time.sleep(3.0)
        if relay_proc.poll() is not None:
            print(f"[python] FAIL: relay exited immediately with "
                  f"{relay_proc.returncode}", file=sys.stderr, flush=True)
            sys.exit(3)
        state["relay_proc"] = relay_proc
    else:
        my_port, my_forward = args.port, args.forward_port

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_cold_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, my_port, my_forward)
    print(f"[python] config dir: {config_dir}", flush=True)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[python] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    # NOTE: no RNS.Destination(..., IN, ...) and no announce() anywhere in
    # this script. This side is invisible to the C++ peer until it speaks.
    RNS.Transport.register_announce_handler(_AnnounceHandler())
    print("[python] listening for the C++ announce; NOT announcing "
          "(this is what makes the test cold)", flush=True)

    payload = build_payload()
    # RNS increments hops on ingress, so the direct case is 1, not 0; the relay
    # adds exactly one more. Both are observable in the C++ log line, so a
    # change in either implementation shows up here as a mismatch with a stated
    # expected value rather than as a silently-adjusted constant.
    want_hops = args.expect_hops
    if want_hops is None:
        want_hops = 2 if args.relay else 1
    if args.self_test_break == "hops":
        want_hops = 1 if args.relay else 2
        print(f"[python] SELF-TEST BREAK: expecting hops={want_hops}, which is "
              "the count for the other topology; the digest will match and the "
              "hop byte must not", flush=True)
    # The expected ack is always computed over the CORRECT payload, so the
    # payload break shows up as a mismatch rather than as two matching wrongs.
    state["expected_ack"] = expected_ack(payload, want_hops)
    print(f"[python] topology: {'RELAYED (1 transport node)' if args.relay else 'DIRECT'}, "
          f"expecting the C++ side to report hops={want_hops}", flush=True)

    if args.self_test_break == "payload":
        payload = bytes([payload[0] ^ 0x01]) + payload[1:]
        print("[python] SELF-TEST BREAK: flipped one bit of the payload; the "
              "C++ side must reject it", flush=True)
    breaker = None
    if args.self_test_break == "coldness":
        breaker = RNS.Destination(RNS.Identity(), RNS.Destination.IN,
                                  RNS.Destination.SINGLE, APP_NAME, ASPECT)
        state["ignore_dest"] = breaker.hash
        print("[python] SELF-TEST BREAK: will announce from this side "
              "repeatedly; the C++ coldness guard must catch it", flush=True)
    print(f"[python] payload {len(payload)} bytes, "
          f"sha256={hashlib.sha256(payload).hexdigest()}", flush=True)
    print(f"[python] expected ack app_data = "
          f"{state['expected_ack'].hex()}", flush=True)

    start = time.time()
    receipt = None
    last_breaker_announce = 0.0

    while time.time() - start < args.timeout:
        # A single announce at startup isn't enough to exercise the guard:
        # this side comes up first, so the C++ receiver isn't yet listening.
        # A peer whose presence would warm this scenario up announces
        # repeatedly, so the break does too.
        if breaker is not None and time.time() - last_breaker_announce >= 2.0:
            breaker.announce()
            last_breaker_announce = time.time()
            print("[python] SELF-TEST BREAK: announced", flush=True)

        # A relay that dies mid-run would leave the two ends unable to reach
        # each other at all. Fail on it explicitly: a timeout here would read
        # as "the C++ side never answered", which would be the wrong diagnosis.
        rp = state["relay_proc"]
        if rp is not None and rp.poll() is not None:
            print(f"[python] FAILURE the relay exited mid-scenario with "
                  f"{rp.returncode}", flush=True)
            sys.exit(1)

        if state["remote_identity"] is not None and not state["sent"]:
            remote_dest = RNS.Destination(state["remote_identity"],
                                          RNS.Destination.OUT,
                                          RNS.Destination.SINGLE,
                                          APP_NAME, ASPECT)
            if remote_dest.hash != state["remote_dest_hash"]:
                print("[python] FAIL: destination hash recomputed from the "
                      f"announced identity ({remote_dest.hash.hex()}) does not "
                      f"match the announced hash "
                      f"({state['remote_dest_hash'].hex()})", flush=True)
                sys.exit(1)

            packet = RNS.Packet(remote_dest, payload)
            receipt = packet.send()
            if receipt is False or receipt is None:
                print("[python] FAIL: packet.send() returned no receipt",
                      flush=True)
                sys.exit(1)
            receipt.set_timeout(15)
            receipt.set_delivery_callback(on_delivered)
            receipt.set_timeout_callback(on_timeout)
            state["sent"] = True
            print(f"[python] originated packet to {remote_dest.hash.hex()}, "
                  f"{len(payload)} bytes", flush=True)

        if state["proven"] and state["ack_ok"]:
            print("[python] SUCCESS cold-originated packet was proven by the "
                  "C++ peer AND the peer echoed a digest of the correct "
                  "plaintext", flush=True)
            sys.exit(0)
        if state["failed"]:
            print("[python] FAILURE proof did not arrive", flush=True)
            sys.exit(1)

        time.sleep(0.1)

    print(f"[python] TIMEOUT announce_seen={state['remote_identity'] is not None} "
          f"sent={state['sent']} proven={state['proven']} "
          f"ack_ok={state['ack_ok']}", flush=True)
    if state["sent"] and state["proven"] and not state["ack_ok"]:
        if state["hops_wrong"] is not None:
            got, want = state["hops_wrong"]
            # Decryption demonstrably worked here, so blaming the crypto would
            # send the next reader down the wrong path entirely.
            print(f"[python] FAILURE the C++ peer decrypted the plaintext "
                  f"correctly, but the packet reached it over {got} hops and "
                  f"this scenario requires {want} -- the topology under test "
                  f"was not the topology exercised", flush=True)
        else:
            print("[python] FAILURE the C++ peer proved the packet but never "
                  "echoed the digest of the plaintext we sent -- it either "
                  "could not decrypt it, or recovered different bytes",
                  flush=True)
        sys.exit(1)
    sys.exit(2)


if __name__ == "__main__":
    main()
