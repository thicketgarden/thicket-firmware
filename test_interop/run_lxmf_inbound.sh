#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario 2: LXMF DELIVERY INBOUND.
# The Python LXMF reference sends a message to our lxmf.delivery address; the
# C++ side asserts every decoded field.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_lxmf_inbound.sh
#
# To prove the test can fail, without editing anything:
#   ... bash test_interop/run_lxmf_inbound.sh --self-test-break content
#   ... bash test_interop/run_lxmf_inbound.sh --self-test-break field
#   ... bash test_interop/run_lxmf_inbound.sh --self-test-break timestamp

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="lxmf-inbound"
PY_SCRIPT="$HERE/python/lxmf_inbound_sender.py"
CPP_PROJECT="$HERE/lxmf_inbound_receiver"
PY_ARGS="$*"
: "${TIMEOUT_S:=40}"

source "$HERE/scripts/driver.sh"
