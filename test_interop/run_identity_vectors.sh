#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 3: IDENTITY REFERENCE VECTORS.
#
# Two halves that check different things, and both must pass:
#   C++    asserts microReticulum's Identity against fixed reference outputs.
#   Python re-derives those same outputs under the RNS actually installed and
#          diffs them, so the committed constants cannot rot when a pin moves.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_identity_vectors.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_identity_vectors.sh --self-test-break ciphertext
#
# To regenerate the vectors after an RNS or microReticulum bump:
#   PATH="/tmp/rnsvenv/bin:$PATH" python3 test_interop/python/identity_vectors.py \
#       --emit > test_interop/identity_vectors/src/vectors.h

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="identity-vectors"
PY_SCRIPT="$HERE/python/identity_vectors.py"
CPP_PROJECT="$HERE/identity_vectors"
PY_ARGS="--verify $HERE/identity_vectors/src/vectors.h $*"
: "${TIMEOUT_S:=40}"

source "$HERE/scripts/driver.sh"
