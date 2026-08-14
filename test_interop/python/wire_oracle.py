#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Wire oracle, Python half.
#
# Reads the JSON report written by the C++ emitter, decodes each packet's raw
# bytes with the reference implementation's own RNS.Packet, and compares every
# header field against the value the packet was built from.
#
# Two distinct things are checked per case, and they fail differently:
#
#   1. The reference reads back the same field values we packed. A mismatch
#      means our encoding places a field where the reference does not look for
#      it, or does not write it at all.
#   2. The numeric constant we used equals the reference's constant of the same
#      name. A mismatch here means the two implementations have drifted apart
#      on what a name means -- our DATA is not the reference's DATA -- which
#      would still round-trip perfectly between two of our own nodes.
#
# The second check is the reason this file exists. An end-to-end scenario
# cannot see it: both ends agree with each other and disagree with everyone.
#
# Usage: wire_oracle.py REPORT.json

import json
import sys

import RNS


def rns_constant(field, name):
    """The reference's own value for a named constant, or None if it has no
    such name -- which is itself a finding worth reporting."""
    holders = {
        "header_type": RNS.Packet,
        "packet_type": RNS.Packet,
        "destination_type": RNS.Destination,
        "transport_type": RNS.Transport,
    }
    return getattr(holders[field], name, None)


def check(case):
    """Returns a list of human-readable failures for one case."""
    failures = []
    raw = bytes.fromhex(case["hex"])

    packet = RNS.Packet(None, raw)
    if not packet.unpack():
        return [f"the reference could not parse the packet at all ({len(raw)} bytes)"]

    for field in ("header_type", "packet_type", "destination_type", "transport_type"):
        ours = case[field]
        theirs = getattr(packet, field)
        if ours != theirs:
            failures.append(
                f"{field}: we packed {ours}, the reference read {theirs}")

        name = case.get(field + "_name")
        if name is None:
            continue
        expected = rns_constant(field, name)
        if expected is None:
            failures.append(
                f"{field}: the reference has no constant named {name}")
        elif expected != ours:
            failures.append(
                f"{field}: we call {ours} '{name}', the reference calls "
                f"{expected} '{name}'")

    if case["context"] != packet.context:
        failures.append(
            f"context: we packed {case['context']}, "
            f"the reference read {packet.context}")

    dst = packet.destination_hash.hex()
    if case["destination_hash"] != dst:
        failures.append(
            f"destination_hash: we packed {case['destination_hash']}, "
            f"the reference read {dst}")

    if case["transport_id"]:
        tid = packet.transport_id.hex() if packet.transport_id else ""
        if case["transport_id"] != tid:
            failures.append(
                f"transport_id: we packed {case['transport_id']}, "
                f"the reference read {tid or '(none)'}")
    elif packet.transport_id:
        failures.append(
            f"transport_id: we packed none, the reference read "
            f"{packet.transport_id.hex()}")

    # Only meaningful where the payload is not encrypted; the emitter leaves
    # data_hex empty for cases whose body the reference cannot read.
    if case["data_hex"]:
        got = packet.data.hex()
        if case["data_hex"] != got:
            failures.append(
                f"data: we packed {case['data_hex']}, the reference read {got}")

    return failures


def main():
    if len(sys.argv) < 2:
        print("usage: wire_oracle.py REPORT.json", file=sys.stderr)
        return 2

    with open(sys.argv[1]) as f:
        report = json.load(f)

    cases = report["cases"]
    if not cases:
        print("[wire-oracle] the emitter produced no cases", file=sys.stderr)
        return 1

    print(f"[wire-oracle] RNS {RNS.__version__}, {len(cases)} cases")

    bad = 0
    for case in cases:
        failures = check(case)
        if failures:
            bad += 1
            print(f"  FAIL  {case['name']}")
            for line in failures:
                print(f"          {line}")
        else:
            print(f"  ok    {case['name']}")

    if bad:
        print(f"[wire-oracle] {bad}/{len(cases)} cases disagree with RNS "
              f"{RNS.__version__}", file=sys.stderr)
        return 1

    print(f"[wire-oracle] all {len(cases)} cases agree with RNS {RNS.__version__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
