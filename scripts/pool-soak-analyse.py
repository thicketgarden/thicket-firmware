#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Reads a board capture from the -soak build and says whether the RNS
# container pool plateaus or climbs.
#
# The host harness (test_interop/pool_soak) answers the same question for the
# outbound path. It cannot answer it for the inbound path, because with no peer
# nothing is ever delivered and the decrypt path never allocates. This reads
# the board, where a real peer is sending, so both halves get measured.
#
#   python3 scripts/board.py flash wiscore_rak4631-soak --seconds 900 | tee soak.log
#   python3 scripts/pool-soak-analyse.py soak.log
#
# Exits 1 on a climbing curve, 2 if the capture contains nothing to judge.

import sys

# Growth allowances, second half of the run against the first. An allocator
# breathes; these bound what still counts as settled.
MAX_USED_GROWTH_PCT = 5.0
MAX_FRAG_GROWTH_PCT = 50.0

# Below this, a run is too short to show a trend and saying either way would be
# a guess dressed as a measurement.
MIN_SAMPLES = 12


def parse(path):
    rows, unbacked = [], False
    with open(path, errors="replace") as f:
        for line in f:
            i = line.find("SOAK,")
            if i < 0:
                continue
            parts = line[i:].strip().split(",")
            if len(parts) < 8:
                continue
            if parts[1] == "unbacked":
                unbacked = True
                continue
            try:
                rows.append({
                    "s": int(parts[1]), "used": int(parts[2]),
                    "used_n": int(parts[3]), "free": int(parts[4]),
                    "free_n": int(parts[5]), "free_max": int(parts[6]),
                    "frag": float(parts[7]),
                })
            except ValueError:
                continue
    return rows, unbacked


def main():
    if len(sys.argv) < 2:
        print("usage: pool-soak-analyse.py CAPTURE.log", file=sys.stderr)
        return 2

    rows, unbacked = parse(sys.argv[1])

    if unbacked:
        print("[pool-soak] the board reported an UNBACKED pool: the container "
              "allocator\n[pool-soak] is not pool-backed in this build, so every "
              "sample would read zero.", file=sys.stderr)
        return 2

    if len(rows) < MIN_SAMPLES:
        print(f"[pool-soak] only {len(rows)} samples in this capture; at least "
              f"{MIN_SAMPLES} are\n[pool-soak] needed before a trend means "
              f"anything. Capture for longer.", file=sys.stderr)
        return 2

    span = rows[-1]["s"] - rows[0]["s"]
    print(f"[pool-soak] {len(rows)} samples over {span} s on the board")

    warm = rows[len(rows) // 10:]
    half = len(warm) // 2
    first, second = warm[:half], warm[half:]
    mean = lambda xs, k: sum(r[k] for r in xs) / len(xs)

    u1, u2 = mean(first, "used"), mean(second, "used")
    f1, f2 = mean(first, "frag"), mean(second, "frag")
    peak = max(r["frag"] for r in warm)
    high = max(r["used"] for r in rows)

    ug = 100.0 * (u2 - u1) / u1 if u1 else 0.0
    fg = 100.0 * (f2 - f1) / f1 if f1 else 0.0

    print(f"[pool-soak] used:          {u1:9.0f} B -> {u2:9.0f} B   ({ug:+.1f}%)")
    print(f"[pool-soak] fragmentation: {f1:9.2f}%  -> {f2:9.2f}%    ({fg:+.1f}%)")
    print(f"[pool-soak] high-water used: {high} B")
    print(f"[pool-soak] peak fragmentation after warm-up: {peak:.2f}%")

    if ug > MAX_USED_GROWTH_PCT or fg > MAX_FRAG_GROWTH_PCT:
        print("\n[pool-soak] VERDICT: CLIMBING on the board. Sizing the pool "
              "larger only postpones exhaustion.")
        return 1

    print("\n[pool-soak] VERDICT: PLATEAU on the board. The pool can be sized "
          "once against the plateau.")
    print("[pool-soak] NOTE: this is only evidence for the traffic the capture "
          "actually saw.\n[pool-soak] Confirm messages were delivered during "
          "the run before trusting it\n[pool-soak] as coverage of the inbound path.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
