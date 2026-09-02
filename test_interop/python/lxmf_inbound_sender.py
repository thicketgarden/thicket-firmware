#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python LXMF originator for the LXMF-DELIVERY-INBOUND interop scenario.

Sends one LXMF message, via the Python LXMF reference implementation, to the
C++ side's `lxmf.delivery` address. The C++ side asserts the decoded fields;
this side asserts that the message reached state DELIVERED.

That second assertion is worth more here than the equivalent in the cold-packet
scenario. microLXMF only calls `packet.prove()` after it has unpacked the
message, matched the destination and accepted the signature (LXMRouter.cpp, the
OPPORTUNISTIC inbound path). So a DELIVERED state on this side really does mean
the C++ router decrypted, parsed and verified -- unlike a bare RNS PacketReceipt.

Not a cold scenario: LXMF signature validation needs the source identity, so
this side announces its own lxmf.delivery destination first. That's the
reference flow. Coldness is scenario 1's job.

Exit codes:
  0  message reached DELIVERED
  1  message reached FAILED, or an unexpected terminal state
  2  timeout
  3  setup error
"""

import argparse
import os
import sys
import tempfile
import time

try:
    import RNS
    import LXMF
except ImportError as e:                                  # pragma: no cover
    print(f"[python] failed to import RNS/LXMF: {e}", file=sys.stderr)
    sys.exit(3)

# ---------------------------------------------------------------------------
# The contract. Duplicated verbatim in lxmf_inbound_receiver/src/main.cpp.
# ---------------------------------------------------------------------------
TITLE = b"thicket-interop-title"
CONTENT = (b"thicket interop: LXMF delivery inbound, "
           b"originated by the Python reference")
TIMESTAMP = 1750000000.5
FIELD_KEY = 6
FIELD_VALUE = b"thicket-interop-field-value"


def write_config(config_dir: str, port: int, forward_port: int) -> None:
    cfg = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37458
  instance_control_port = 37459
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


state = {"peer_hash": None, "peer_identity": None}


class _DeliveryAnnounceHandler:
    def __init__(self):
        self.aspect_filter = "lxmf.delivery"

    def received_announce(self, destination_hash, announced_identity, app_data):
        if destination_hash == state.get("own_hash"):
            return
        if state["peer_hash"] is not None:
            return
        state["peer_hash"] = destination_hash
        state["peer_identity"] = announced_identity
        print(f"[python] learned C++ lxmf.delivery: {destination_hash.hex()}",
              flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=14272)
    ap.add_argument("--forward-port", type=int, default=14273)
    ap.add_argument("--timeout", type=float, default=40.0)
    # See cold_inbound_sender.py for the rationale: a proof of failure that
    # anyone can re-run beats a proof of failure written down in a report.
    ap.add_argument("--self-test-break",
                    choices=("none", "content", "field", "timestamp"),
                    default="none",
                    help="deliberately break one assertion, to prove it is live")
    args = ap.parse_args()

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_lxmf_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.port, args.forward_port)
    print(f"[python] config dir: {config_dir}", flush=True)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[python] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    identity = RNS.Identity()
    router = LXMF.LXMRouter(identity=identity, storagepath=config_dir)
    source = router.register_delivery_identity(identity,
                                               display_name="python-reference")
    state["own_hash"] = source.hash
    print(f"[python] own lxmf.delivery: {source.hash.hex()}", flush=True)

    RNS.Transport.register_announce_handler(_DeliveryAnnounceHandler())

    title = TITLE
    content = CONTENT
    timestamp = TIMESTAMP
    fields = {FIELD_KEY: FIELD_VALUE}

    if args.self_test_break == "content":
        content = CONTENT[:-1] + bytes([CONTENT[-1] ^ 0x01])
        print("[python] SELF-TEST BREAK: altered one byte of the content",
              flush=True)
    elif args.self_test_break == "field":
        fields = {FIELD_KEY + 1: FIELD_VALUE}
        print("[python] SELF-TEST BREAK: moved the field to key "
              f"{FIELD_KEY + 1}", flush=True)
    elif args.self_test_break == "timestamp":
        timestamp = TIMESTAMP + 1.0
        print("[python] SELF-TEST BREAK: shifted the timestamp by 1s",
              flush=True)

    start = time.time()
    sent = False
    message = None
    last_announce = 0.0
    send_after = None

    while time.time() - start < args.timeout:
        now = time.time()

        # Announce our own delivery destination so the C++ router can recall
        # our identity and validate the signature. Without this the message is
        # accepted but flagged SOURCE_UNKNOWN and signature_validated is False.
        if now - last_announce >= 1.5:
            router.announce(source.hash)
            last_announce = now

        # Do NOT send on first sight of the peer. This side starts first, so
        # our early announces went out before the C++ receiver was listening;
        # it has heard us only from the next announce onward. Wait until we
        # have announced at least twice with the peer known to be up,
        # otherwise the message arrives before the identity does and the
        # signature can't be validated on receipt.
        if state["peer_hash"] is not None and send_after is None:
            router.announce(source.hash)
            last_announce = now
            send_after = now + 3.0
            print("[python] peer is up; announcing before sending so the "
                  "source identity is known when the message lands",
                  flush=True)

        if send_after is not None and now >= send_after and not sent:
            dest = RNS.Destination(state["peer_identity"], RNS.Destination.OUT,
                                   RNS.Destination.SINGLE, "lxmf", "delivery")
            message = LXMF.LXMessage(dest, source, content, title,
                                     fields=fields,
                                     desired_method=LXMF.LXMessage.OPPORTUNISTIC)
            # LXMessage.pack() only stamps time.time() when timestamp is None,
            # so pinning it here makes the C++ assertion exact.
            message.timestamp = timestamp
            router.handle_outbound(message)
            sent = True
            print(f"[python] sent LXMF message to {dest.hash.hex()}",
                  flush=True)
            print(f"[python]   title      {title!r}", flush=True)
            print(f"[python]   content    {len(content)} bytes", flush=True)
            print(f"[python]   timestamp  {timestamp!r}", flush=True)
            print(f"[python]   fields     {fields!r}", flush=True)

        if message is not None:
            if message.state == LXMF.LXMessage.DELIVERED:
                print("[python] SUCCESS message state DELIVERED -- the C++ "
                      "router unpacked, matched the destination and accepted "
                      "the signature", flush=True)
                sys.exit(0)
            if message.state == LXMF.LXMessage.FAILED:
                print("[python] FAILURE message state FAILED", flush=True)
                sys.exit(1)

        time.sleep(0.1)

    st = message.state if message is not None else None
    print(f"[python] TIMEOUT peer_seen={state['peer_hash'] is not None} "
          f"sent={sent} state={st}", flush=True)
    sys.exit(2)


if __name__ == "__main__":
    main()
