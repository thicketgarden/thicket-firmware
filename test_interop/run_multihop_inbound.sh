#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 5: MULTI-HOP INBOUND.
#
# Same C++ leaf binary and same payload as the cold-inbound scenario, but with
# a transport-enabled Python RNS node inserted between the two ends:
#
#   Python originator <--UDP--> Python relay <--UDP--> C++ leaf
#      (transport off)        (enable_transport=Yes)   (transport off)
#
# What this covers that nothing else did: every other scenario puts the two
# implementations on the same wire, so the C++ side has only ever handled
# packets that arrived directly. This is the first exercise of a packet that
# reached us THROUGH a transport node -- the path is learned from a relayed
# announce, and the leaf must handle a non-zero hop count and a destination it
# can only reach via that path.
#
# It is also the only interop coverage of the file that carries all four of our
# local patches, which is why the parity matrix listed it as untested.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_multihop_inbound.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_multihop_inbound.sh --self-test-break hops
#   ... bash test_interop/run_multihop_inbound.sh --self-test-break payload

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="multihop-inbound"
PY_SCRIPT="$HERE/python/cold_inbound_sender.py"
CPP_PROJECT="$HERE/cold_inbound_receiver"
PY_ARGS="--relay $*"
# Three processes to bring up rather than two, and the path has to be learned
# through the relay before the first packet can be sent.
: "${TIMEOUT_S:=60}"

source "$HERE/scripts/driver.sh"
