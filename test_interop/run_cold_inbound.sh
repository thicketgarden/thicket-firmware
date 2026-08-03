#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 1: COLD INBOUND.
# A Python RNS peer originates an encrypted packet to a C++ destination that
# has announced but has never transmitted to it. See
# cold_inbound_receiver/src/main.cpp for why this is the gap that mattered.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_cold_inbound.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_cold_inbound.sh --self-test-break payload
#   ... bash test_interop/run_cold_inbound.sh --self-test-break coldness

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="cold-inbound"
PY_SCRIPT="$HERE/python/cold_inbound_sender.py"
CPP_PROJECT="$HERE/cold_inbound_receiver"
PY_ARGS="$*"
: "${TIMEOUT_S:=40}"

source "$HERE/scripts/driver.sh"
