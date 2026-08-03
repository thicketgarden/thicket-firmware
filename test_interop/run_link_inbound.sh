#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 4: LINK INBOUND, KEEPALIVE AND TEARDOWN UNDER LOSS.
# Python establishes a Link to a C++ destination, exchanges data over it,
# idles it past several keepalive intervals, then cuts the wire through a UDP
# relay and watches the reference's watchdog time the link out.
#
# Runs for about a minute -- the phases are timed, not event-driven.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_link_inbound.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_link_inbound.sh --self-test-break payload
#   ... bash test_interop/run_link_inbound.sh --self-test-break nocut

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="link-inbound"
PY_SCRIPT="$HERE/python/link_inbound_initiator.py"
CPP_PROJECT="$HERE/link_inbound_responder"
PY_ARGS="$*"
: "${TIMEOUT_S:=100}"

source "$HERE/scripts/driver.sh"
