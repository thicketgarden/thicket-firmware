#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario: interoperate with the reference implementation's OWN example.
#
# Every other scenario in this directory talks to a Python script we wrote,
# which only shows that we agree with our own reading of the protocol. This one
# runs `Examples/Echo.py` out of the Reticulum source tree, unmodified, at the
# version of RNS installed here.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_examples_echo.sh
#
# Needs network on the first run: it clones the reference repository at the
# matching tag to get Examples/.

set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="examples-echo"
PY_SCRIPT="$HERE/python/examples_echo_server.py"
CPP_PROJECT="$HERE/examples_echo"
: "${TIMEOUT_S:=90}"

source "$HERE/scripts/driver.sh"
