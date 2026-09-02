#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Python link initiator for the LINK-INBOUND interop scenario.

Closes two gaps at once.

  Direction. microReticulum's link scenario is C++ -> Python only. Here the
  Python reference establishes the Link TO a C++ destination, which is the
  direction a handheld is actually on the receiving end of.

  Keepalive and teardown under loss. All traffic runs through
  an in-process UDP relay this script controls, so the test can cut the wire at
  a chosen moment rather than waiting for a real outage.

Timings. RNS ships KEEPALIVE=360s and STALE_TIME=720s, which would make this a
twenty-minute test. Both are per-Link instance attributes in the reference
(Link.__init__ sets self.keepalive / self.stale_time from the class defaults),
so this script shortens them on its own link only. Nothing in the C++ side is
touched, and the behaviour under test -- does the peer answer keepalives, does
the initiator time the link out when it stops -- is unchanged.

Phases:
  1. Learn the C++ destination from its announce and establish a Link.
  2. Send 200 bytes over the link; require the C++ side to echo them back.
  3. Idle past several keepalive intervals with the channel healthy. The link
     may only survive this if the C++ side is ANSWERING keepalives, because
     our own watchdog closes it at stale_time otherwise.
  4. Cut the wire. Require our watchdog to close the link with TIMEOUT inside
     the stale window.

Exit codes:
  0  every phase passed
  1  an assertion failed
  2  timeout waiting for the peer
  3  setup error
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import RNS
except ImportError as e:                                  # pragma: no cover
    print(f"[python] failed to import RNS: {e}", file=sys.stderr)
    sys.exit(3)

from lossy_relay import LossyRelay

APP_NAME = "thicket_interop"
ASPECT = "link"

PAYLOAD_LEN = 200

# Short enough to run in under a minute, long enough to be several multiples of
# the round trip on loopback.
KEEPALIVE_S = 3.0
STALE_TIME_S = 12.0
IDLE_OBSERVE_S = 15.0    # > 4 keepalive intervals and > stale_time

# WHERE THESE HAVE TO BE SET, AND WHY IT IS NOT OBVIOUS
#
# RNS recomputes both timers from the measured RTT when the link goes ACTIVE:
#
#   Link.__update_keepalive():
#       self.keepalive   = clamp(rtt * (KEEPALIVE_MAX / KEEPALIVE_MAX_RTT), 5, 360)
#       self.stale_time  = self.keepalive * STALE_FACTOR
#
# with KEEPALIVE_MAX=360, KEEPALIVE_MAX_RTT=1.75 -- a multiplier of ~205.7 --
# and it's called from link establishment (Link.py:433 and :530), which happens
# AFTER the constructor returns. So assigning these on the fresh Link object,
# as this script did until 2026-08-05, is overwritten before the first phase
# runs. Measured on a passing local run: the script asked for 3.0/12.0 and the
# link actually ran at keepalive=5, stale_time=10 -- the KEEPALIVE_MIN floor,
# not our value.
#
# That's why the scenario flaked in CI. On loopback the multiplier lands under
# the floor and the effective timers are a constant 5/10, so the run looks
# stable. On a loaded shared runner the RTT rises, the floor stops applying and
# the timers scale linearly with it: an RTT of 0.25s gives keepalive=51s and
# stale_time=103s. The 2026-08-05 failure closed nothing in 50s and the relay
# logged exactly one dropped packet, which is a single keepalive -- both are the
# signature of that scaling, not of a defect in our C++ side.
#
# The earlier response was to widen CUT_OBSERVE_S from 25 to 50. That treated a
# wrong timer as a slow machine, and it failed again at 50.1s because no fixed
# window can outrun a value that scales with load. The timers are now pinned
# after establishment, where nothing overwrites them, and verified rather than
# assumed -- see assert_timers_pinned().

# How long to allow for the reference's watchdog to close the link after the
# wire is cut. With the timers actually pinned the close is stale_time +
# rtt*KEEPALIVE_TIMEOUT_FACTOR + STALE_GRACE, so about 17s. The window stays
# generous because the exact latency isn't the property under test -- the
# assertion is that the reference closes the link at all, in contrast to our
# C++ side, which has no Link watchdog (the XFAIL below). The observed close
# time is printed either way, so a real latency regression stays visible.
CUT_OBSERVE_S = 45.0


def assert_timers_pinned(link):
    """Fail loudly if RNS isn't running the timers this scenario asked for.

    Every timing assertion below is meaningless if these don't hold, and the
    failure mode is silent: the run still passes locally, because on loopback
    the value RNS substitutes happens to be close enough. Checked rather than
    trusted, so a future RNS that recomputes them somewhere new is caught here
    with a clear message instead of as an unexplained timeout minutes later.
    """
    print(f"[python] effective link timers: rtt={link.rtt * 1000:.1f}ms "
          f"keepalive={link.keepalive}s stale_time={link.stale_time}s",
          flush=True)
    if (link.keepalive, link.stale_time) != (KEEPALIVE_S, STALE_TIME_S):
        print(f"[python] FAILURE RNS is running keepalive={link.keepalive}s "
              f"stale_time={link.stale_time}s, not the {KEEPALIVE_S}s/"
              f"{STALE_TIME_S}s this scenario set. Something recomputed them "
              f"after establishment; every timing check below would be "
              f"measuring RNS's own scaling rather than the peer.", flush=True)
        sys.exit(1)


def build_payload(n: int = PAYLOAD_LEN) -> bytes:
    out = bytearray()
    x = 0x11C0FFEE
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


state = {
    "peer_identity": None,
    "peer_hash": None,
    "link": None,
    "established": False,
    "echo_ok": False,
    "echo_mismatch": False,
    "closed": False,
    "close_reason": None,
}

checks = 0
failures = 0


def check(ok, what, detail=""):
    global checks, failures
    checks += 1
    if ok:
        print(f"[python]   OK   {what:<28} {detail}", flush=True)
    else:
        failures += 1
        print(f"[python]   FAIL {what:<28} {detail}", flush=True)


class _AnnounceHandler:
    def __init__(self):
        self.aspect_filter = f"{APP_NAME}.{ASPECT}"

    def received_announce(self, destination_hash, announced_identity, app_data):
        if state["peer_identity"] is not None:
            return
        state["peer_identity"] = announced_identity
        state["peer_hash"] = destination_hash
        print(f"[python] learned C++ destination: {destination_hash.hex()}",
              flush=True)


def on_established(link):
    state["established"] = True
    print(f"[python] link established: {link.link_id.hex()}", flush=True)


def on_closed(link):
    state["closed"] = True
    state["close_reason"] = link.teardown_reason
    print(f"[python] link closed, teardown_reason={link.teardown_reason}",
          flush=True)


def on_link_packet(data, packet):
    expected = build_payload()
    if data == expected:
        state["echo_ok"] = True
        print(f"[python] echo received over link: {len(data)} bytes, match=True",
              flush=True)
    else:
        state["echo_mismatch"] = True
        print(f"[python] echo MISMATCH: {len(data)} bytes", flush=True)


def write_config(config_dir: str, port: int, forward_port: int) -> None:
    cfg = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37468
  instance_control_port = 37469
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


def wait_until(predicate, seconds, tick=0.1):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(tick)
    return predicate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=14292)
    ap.add_argument("--relay-to-cpp", type=int, default=14290)
    ap.add_argument("--relay-to-python", type=int, default=14291)
    ap.add_argument("--cpp-port", type=int, default=14293)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--self-test-break",
                    choices=("none", "payload", "nocut"), default="none",
                    help="deliberately break one assertion, to prove it is live")
    args = ap.parse_args()

    # Relay sits between the two RNS instances. Leg A carries C++ -> Python,
    # leg B carries Python -> C++.
    relay = LossyRelay(a_listen=args.relay_to_cpp, a_forward=args.port,
                       b_listen=args.relay_to_python, b_forward=args.cpp_port)
    relay.start()

    config_dir = tempfile.mkdtemp(prefix="thicket_interop_link_")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir, args.port, args.relay_to_python)
    print(f"[python] config dir: {config_dir}", flush=True)

    try:
        RNS.Reticulum(config_dir)
    except Exception as e:
        print(f"[python] Reticulum init failed: {e}", file=sys.stderr)
        sys.exit(3)

    RNS.Transport.register_announce_handler(_AnnounceHandler())

    if not wait_until(lambda: state["peer_identity"] is not None, 30.0):
        print("[python] TIMEOUT waiting for the C++ announce", flush=True)
        relay.stop()
        sys.exit(2)

    dest = RNS.Destination(state["peer_identity"], RNS.Destination.OUT,
                           RNS.Destination.SINGLE, APP_NAME, ASPECT)

    print("[python] --- phase 1: establish ---", flush=True)
    link = RNS.Link(dest, established_callback=on_established,
                    closed_callback=on_closed)
    state["link"] = link
    link.set_packet_callback(on_link_packet)

    check(wait_until(lambda: state["established"], 20.0),
          "link established", f"status={link.status}")
    if not state["established"]:
        relay.stop()
        print("[python] FAILURE could not establish the link", flush=True)
        sys.exit(1)

    # Only now. RNS overwrote these at establishment -- see the note by the
    # constants. Per-instance, so the class defaults and every other link in
    # the process are untouched.
    link.keepalive = KEEPALIVE_S
    link.stale_time = STALE_TIME_S
    assert_timers_pinned(link)

    print("[python] --- phase 2: data over the link ---", flush=True)
    payload = build_payload()
    if args.self_test_break == "payload":
        payload = bytes([payload[0] ^ 0x01]) + payload[1:]
        print("[python] SELF-TEST BREAK: flipped one bit of the link payload; "
              "the C++ side must refuse to echo it", flush=True)
    RNS.Packet(link, payload).send()
    print(f"[python] sent {len(payload)} bytes over the link", flush=True)
    check(wait_until(lambda: state["echo_ok"], 15.0) and not state["echo_mismatch"],
          "peer echoed link data",
          "200 bytes round-tripped" if state["echo_ok"] else "no matching echo")

    print(f"[python] --- phase 3: idle {IDLE_OBSERVE_S}s "
          f"(keepalive={KEEPALIVE_S}s, stale_time={STALE_TIME_S}s) ---",
          flush=True)
    # If the C++ side didn't answer keepalives, our own watchdog would mark
    # the link stale and close it inside stale_time. Surviving this window is
    # the assertion.
    idle_survived = not wait_until(lambda: state["closed"], IDLE_OBSERVE_S)
    check(idle_survived, "link survived idle",
          f"status={link.status} after {IDLE_OBSERVE_S}s "
          f"({IDLE_OBSERVE_S / KEEPALIVE_S:.0f} keepalive intervals, "
          f"stale_time {STALE_TIME_S}s); only possible if the peer answered "
          f"keepalives"
          if idle_survived else
          f"closed during idle, reason={state['close_reason']}")
    if not idle_survived:
        relay.stop()
        print("[python] FAILURE link did not survive an idle period", flush=True)
        sys.exit(1)

    print("[python] --- phase 4: cut the wire ---", flush=True)
    if args.self_test_break == "nocut":
        print("[python] SELF-TEST BREAK: NOT cutting the wire; the timeout "
              "assertion must fail because the link stays up", flush=True)
    else:
        relay.cut = True
        print(f"[python] relay cut (forwarded={relay.forwarded} so far); "
              f"expecting our watchdog to close the link within "
              f"~{STALE_TIME_S}s + grace", flush=True)

    cut_at = time.time()
    closed = wait_until(lambda: state["closed"], CUT_OBSERVE_S)
    elapsed = time.time() - cut_at
    check(closed, "link timed out after loss",
          f"closed after {elapsed:.1f}s, reason={state['close_reason']} "
          f"(TIMEOUT={RNS.Link.TIMEOUT})" if closed else
          f"still {link.status} after {elapsed:.1f}s")
    if closed:
        # Widening the window (above) must not hide a latency regression, so
        # say so loudly when the close is far slower than the protocol implies
        # without failing the run for it.
        expected = STALE_TIME_S + 3.0
        if elapsed > expected * 2:
            print(f"[python] NOTE: watchdog took {elapsed:.1f}s to close; "
                  f"stale_time+grace implies ~{expected:.0f}s. Within the "
                  f"allowance, but worth a look if it persists.", flush=True)
        check(state["close_reason"] == RNS.Link.TIMEOUT,
              "teardown reason is TIMEOUT",
              f"got {state['close_reason']}, want {RNS.Link.TIMEOUT}")

    print(f"[python] relay stats: forwarded={relay.forwarded} "
          f"dropped={relay.dropped}", flush=True)
    relay.stop()

    if failures:
        print(f"[python] FAILURE {failures} of {checks} link checks failed",
              flush=True)
        sys.exit(1)
    print(f"[python] SUCCESS all {checks} link checks passed", flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
