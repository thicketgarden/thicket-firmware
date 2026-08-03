#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python originator for the COLD INBOUND interop scenario.

This is the half of the scenario that makes it cold. It:

  1. Brings up RNS on a loopback UDP interface.
  2. **Never announces.** The C++ peer therefore cannot learn this side's
     identity and cannot have transmitted to it. That is the whole point:
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
        sha256(payload || b"thicket-cold-ack")[:16]. Only a peer that
        actually recovered the plaintext can produce that, so this is the
        assertion that the cold decrypt really happened.

Exit codes:
  0  packet sent AND proof validated AND plaintext digest confirmed
  1  proof failed, or the digest never appeared / did not match
  2  timeout waiting for the C++ announce
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


def expected_ack(payload: bytes) -> bytes:
    return hashlib.sha256(payload + ACK_TAG).digest()[:16]


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
    # A test that has never been seen to fail is not evidence (T28). These
    # switches make the failure re-runnable by anyone, at any time, instead of
    # living in a report someone has to trust. They are opt-in and default off.
    #   payload   -- flip one byte of the payload on the wire; the C++ side
    #                must notice and refuse to ack.
    #   coldness  -- announce from this side, which is exactly the condition
    #                that would silently turn this back into the already-covered
    #                warm case; the C++ coldness guard must catch it.
    ap.add_argument("--self-test-break", choices=("none", "payload", "coldness"),
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

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_cold_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.port, args.forward_port)
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
    # The expected ack is always computed over the CORRECT payload, so the
    # payload break shows up as a mismatch rather than as two matching wrongs.
    state["expected_ack"] = expected_ack(payload)

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
        # A single announce at startup is not enough to exercise the guard:
        # this side comes up first, so the C++ receiver is not yet listening.
        # A peer whose presence would really warm this scenario up announces
        # repeatedly, so the break does too.
        if breaker is not None and time.time() - last_breaker_announce >= 2.0:
            breaker.announce()
            last_breaker_announce = time.time()
            print("[python] SELF-TEST BREAK: announced", flush=True)

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
        print("[python] FAILURE the C++ peer proved the packet but never "
              "echoed the digest of the plaintext we sent -- it either could "
              "not decrypt it, or recovered different bytes", flush=True)
        sys.exit(1)
    sys.exit(2)


if __name__ == "__main__":
    main()
