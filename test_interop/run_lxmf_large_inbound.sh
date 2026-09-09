#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# LXMF LARGE INBOUND. The Python LXMF reference sends a ~16 KB message via
# DIRECT (link) delivery, which the reference packs and sends as a bz2-
# compressed multi-packet Resource. The C++ side receives it over a live link,
# decompresses it in Resource::assemble, delivers it, and asserts the content
# matches byte for byte. This exercises the on-device bz2 receive path inside
# the live LXMF flow (link, store write, delivery proof), not beside an idle
# probe, and it guards the large-inbound path on every pin bump.
#
# It proves correctness and concurrency, NOT the device RAM margin: this runs
# native, where the heap is unbounded. The heap peak that decides the bz2 cap
# needs the board, driven by this same sender over an rnsd bridge.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_lxmf_large_inbound.sh
#
# To prove the content assertion is live, without editing anything:
#   ... bash test_interop/run_lxmf_large_inbound.sh --self-test-break content

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="lxmf-large-inbound"
PY_SCRIPT="$HERE/python/lxmf_inbound_sender.py"
CPP_PROJECT="$HERE/lxmf_inbound_receiver"

# Size the message once, on both sides. The C++ receiver regenerates the same
# bytes from this env var; the sender generates and sends them.
: "${LXMF_CONTENT_SIZE:=16384}"
export THICKET_LXMF_CONTENT_SIZE="$LXMF_CONTENT_SIZE"

PY_ARGS="--content-size $LXMF_CONTENT_SIZE --method direct $*"
# Link establishment plus a multi-packet resource transfer needs a little more
# than the single-packet scenario's window.
: "${TIMEOUT_S:=60}"

source "$HERE/scripts/driver.sh"
