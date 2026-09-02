#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 7: ENCRYPTED-AT-REST MESSAGE STORAGE, BOTH DIRECTIONS.
#
# Two halves that check opposite directions, and both must pass:
#   C++    decrypts a 92-byte file upstream's own device wrote, plus negative
#          cases (tampered body, altered version byte, wrong identity,
#          truncation). Proves we can READ the format.
#   Python re-implements the format from its specification, on a different
#          crypto library, and opens a file the C++ side just wrote. Proves
#          something other than us can read what we WRITE.
#
# The second half isn't redundant. Our reader would accept our writer's output
# even if both were wrong the same way -- a round trip can't detect a shared
# mistake, which is the whole reason this is an interop scenario and not a unit
# test.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_encstore_vectors.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_encstore_vectors.sh --self-test-break tamper

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

# A directory both sides can see. driver.sh runs the C++ side in its own scratch
# CWD, so a shared path has to be passed rather than assumed.
XDIR="$(mktemp -d -t thicket_encstore.XXXXXX)"
export THICKET_ENCSTORE_DIR="$XDIR"
trap 'rm -rf "$XDIR"' EXIT

SCENARIO="encstore-vectors"
PY_SCRIPT="$HERE/python/encstore_vectors.py"
CPP_PROJECT="$HERE/encstore_vectors"
PY_ARGS="--dir $XDIR $*"
: "${TIMEOUT_S:=40}"

source "$HERE/scripts/driver.sh"
