#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario: WIRE ORACLE.
#
# Sequential rather than concurrent, so it doesn't use scripts/driver.sh:
#   C++    packs a set of packets and writes them out as hex with the field
#          values each was built from.
#   Python decodes those same bytes with the reference implementation's own
#          RNS.Packet and compares every field.
#
# What this catches that an end-to-end scenario can't: an encoding both of our
# own ends agree on and the rest of the network doesn't. A delivery test
# passes in that case; this one doesn't.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_wire_oracle.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_wire_oracle.sh --self-test-break

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

CPP_PROJECT="$HERE/wire_oracle"
CPP_BIN="$CPP_PROJECT/.pio/build/native17/program"
PY_SCRIPT="$HERE/python/wire_oracle.py"
REPORT="${REPORT:-${TMPDIR:-/tmp}/thicket-wire-oracle.json}"

SELF_TEST_BREAK=0
for arg in "$@"; do
	[ "$arg" = "--self-test-break" ] && SELF_TEST_BREAK=1
done

if [ ! -x "$CPP_BIN" ]; then
	echo "[wire-oracle] building the emitter" >&2
	( cd "$CPP_PROJECT" && pio run -e native17 ) >/dev/null 2>&1 || {
		echo "[wire-oracle] emitter failed to build; run:" >&2
		echo "[wire-oracle]   cd $CPP_PROJECT && pio run -e native17" >&2
		exit 1
	}
fi

if ! "$CPP_BIN" "$REPORT" >/dev/null 2>&1; then
	echo "[wire-oracle] the emitter did not produce a report" >&2
	exit 1
fi

if [ "$SELF_TEST_BREAK" = "1" ]; then
	echo "[wire-oracle] --self-test-break: corrupting one context byte" >&2
	python3 - "$REPORT" <<'PY'
import json, sys
path = sys.argv[1]
report = json.load(open(path))
case = report["cases"][0]
raw = bytearray(bytes.fromhex(case["hex"]))
raw[18] ^= 0x09          # the context byte of a HEADER_1 packet
case["hex"] = raw.hex()
json.dump(report, open(path, "w"))
PY
fi

python3 "$PY_SCRIPT" "$REPORT"
RC=$?

if [ "$SELF_TEST_BREAK" = "1" ]; then
	if [ "$RC" = "0" ]; then
		echo "[wire-oracle] SELF-TEST FAILED: a corrupted packet was accepted" >&2
		exit 1
	fi
	echo "[wire-oracle] self-test passed: the corruption was caught" >&2
	exit 0
fi

exit "$RC"
