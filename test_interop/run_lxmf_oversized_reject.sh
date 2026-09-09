#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# LXMF OVERSIZED-RESOURCE REJECT. Proves the accept-size guard. The C++ side is
# built with a small accept cap (-DRNS_RESOURCE_MAX_SIZE=4096, in this scenario's
# own platformio.ini). The Python peer sends an 8 KB INCOMPRESSIBLE message via
# DIRECT delivery, so RNS sends it as an uncompressed multi-packet Resource whose
# transfer size exceeds the cap. The device must reject it at accept (RESOURCE_RCL),
# never deliver it, and not fault. Without the guard the parts would fill the pool
# and throw mid-reception where nothing catches it, resetting a real board.
#
# A PASS: the sender's message reaches FAILED (rejected) and the C++ side sees no
# delivery and exits 0. Both are inverted from the normal inbound scenario, which
# is the point.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_lxmf_oversized_reject.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="lxmf-oversized-reject"
PY_SCRIPT="$HERE/python/lxmf_inbound_sender.py"
CPP_PROJECT="$HERE/lxmf_oversized_reject"

export THICKET_LXMF_EXPECT_REJECT=1
PY_ARGS="--content-size 8192 --incompressible --method direct --expect-reject $*"
: "${TIMEOUT_S:=40}"

source "$HERE/scripts/driver.sh"
