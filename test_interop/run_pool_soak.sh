#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario: POOL SOAK.
#
# Drives message traffic through the real LXMRouter and samples the RNS
# container pool every cycle, to establish whether fragmentation plateaus or
# climbs. Sized for a quick run by default; pass a cycle count for a long one.
#
#   bash test_interop/run_pool_soak.sh [CYCLES]
#
# The verdict is printed at the end. A climbing curve exits non-zero; a plateau
# exits 0. What counts as a climb is stated in the thresholds below rather than
# left to the reader.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

CYCLES="${1:-2000}"
CPP_PROJECT="$HERE/pool_soak"
CPP_BIN="$CPP_PROJECT/.pio/build/native17/program"
CSV="${CSV:-${TMPDIR:-/tmp}/thicket-pool-soak.csv}"

# A plateau isn't "perfectly flat" -- allocators breathe. These bound what
# counts as flat: the second half of the run must not use materially more than
# the first, and fragmentation must not trend upward across it.
MAX_USED_GROWTH_PCT="${MAX_USED_GROWTH_PCT:-5}"
MAX_FRAG_GROWTH_PCT="${MAX_FRAG_GROWTH_PCT:-50}"

if [ ! -x "$CPP_BIN" ]; then
	echo "[pool-soak] building" >&2
	( cd "$CPP_PROJECT" && pio run -e native17 ) >/dev/null 2>&1 || {
		echo "[pool-soak] build failed; rerun to see why:" >&2
		echo "[pool-soak]   cd $CPP_PROJECT && pio run -e native17" >&2
		exit 1
	}
fi

"$CPP_BIN" "$CYCLES" "$CSV" || exit $?

python3 - "$CSV" "$MAX_USED_GROWTH_PCT" "$MAX_FRAG_GROWTH_PCT" <<'PY'
import csv, sys

rows = list(csv.DictReader(open(sys.argv[1])))
max_used_growth = float(sys.argv[2])
max_frag_growth = float(sys.argv[3])

if len(rows) < 200:
    print(f"[pool-soak] only {len(rows)} cycles; too few to call a trend.")
    sys.exit(0)

# Discard the opening ramp: caches fill to their bound early and that rise is
# not the thing under test.
warm = rows[len(rows) // 10:]
half = len(warm) // 2
first, second = warm[:half], warm[half:]

def mean(sample, key):
    return sum(float(r[key]) for r in sample) / len(sample)

u1, u2 = mean(first, "used_size"), mean(second, "used_size")
f1, f2 = mean(first, "frag_pct"), mean(second, "frag_pct")
peak = max(float(r["frag_pct"]) for r in warm)

used_growth = 100.0 * (u2 - u1) / u1 if u1 else 0.0
frag_growth = 100.0 * (f2 - f1) / f1 if f1 else 0.0

print()
print("[pool-soak] === trend, first half vs second half (after warm-up) ===")
print(f"[pool-soak] used:          {u1:9.0f} B -> {u2:9.0f} B   ({used_growth:+.1f}%)")
print(f"[pool-soak] fragmentation: {f1:9.2f}%  -> {f2:9.2f}%    ({frag_growth:+.1f}%)")
print(f"[pool-soak] peak fragmentation after warm-up: {peak:.2f}%")

climbing = used_growth > max_used_growth or frag_growth > max_frag_growth
if climbing:
    print()
    print("[pool-soak] VERDICT: CLIMBING. The pool does not settle on this "
          "workload; sizing it larger only postpones exhaustion.")
    sys.exit(1)

print()
print("[pool-soak] VERDICT: PLATEAU. Use and fragmentation settle, so the pool "
      "can be sized once against the plateau.")
PY
