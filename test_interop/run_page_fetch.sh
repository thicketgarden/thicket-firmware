#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# NomadNet PAGE FETCH. A Python NomadNet node serves a known page through a
# registered request handler; the C++ side discovers it by announce, establishes
# a Link, requests the page, and verifies the Micron returns byte-for-byte. This
# is the desktop browser's fetch edge reduced to the wire.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_page_fetch.sh
#
# A PASS needs both sides to exit 0 and print SUCCESS.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

SCENARIO="page-fetch"
PY_SCRIPT="$HERE/python/page_fetch_server.py"
CPP_PROJECT="$HERE/page_fetch_requester"
PY_ARGS="$*"
: "${TIMEOUT_S:=30}"

source "$HERE/scripts/driver.sh"
