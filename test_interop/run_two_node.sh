#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 8: TWO OF OUR OWN NODES — no reference in the middle.
#
# The only scenario where our stack is on BOTH ends. It exists because every
# other one has the reference originate and us receive, so our outbound
# constructs are validated only by whatever the reference happens to accept.
# Here we must parse what we emit.
#
# ⚠ NOT a conformance result, and it must never be cited as one. Two
# implementations that misread the protocol identically agree perfectly. This
# supplements the suite; it replaces nothing.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_two_node.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_two_node.sh --self-test-break payload

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="two-node"
CPP_PROJECT="$HERE/two_node"

# Two C++ processes rather than one C++ and one Python -- see driver.sh. The
# receiver comes up first because it is the one that announces; the originator
# cannot do anything until it has heard that.
export PEER_CPP_ROLE="receiver"
export CPP_ROLE="originator"

for arg in "$@"; do
  case "$arg" in
    --self-test-break) shift ;;
    payload) export THICKET_BREAK_PAYLOAD=1; shift ;;
  esac
done

: "${TIMEOUT_S:=40}"
source "$HERE/scripts/driver.sh"
