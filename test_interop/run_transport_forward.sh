#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 6: TRANSPORT FORWARDING: the only one where WE are the router.
#
#   python originator            C++ node            python far end
#   14280 <---------------> 14281 | 14283 <---------------> 14282
#            segment A                       segment B
#
# The two Python peers share no interface and can't hear each other. A payload
# arriving at the far end is therefore proof that the C++ node forwarded it, and
# the hop count is proof of how: RNS counts a hop on ingress at every node, so a
# packet that really crossed the router arrives counted twice.
#
# This is the configuration none of the other five scenarios touch. They all run
# transport_enabled(false) and put our stack at the edge; Transport.cpp carries
# all four of our local patches and had no coverage as a router at all.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_transport_forward.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_transport_forward.sh --self-test-break nohop

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="transport-forward"
PY_SCRIPT="$HERE/python/forward_originator.py"
CPP_PROJECT="$HERE/transport_forwarder"
PY_ARGS="$*"
# Three processes, and the router has to learn a path before anything can be
# sent, so this is the slowest scenario in the suite.
: "${TIMEOUT_S:=70}"

source "$HERE/scripts/driver.sh"
