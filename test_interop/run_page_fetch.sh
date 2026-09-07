#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# NomadNet PAGE FETCH. A Python NomadNet node serves a real page
# (desktop/pages/index.mu) at two paths: /page/index.mu uncompressed, and
# /page/gz.mu with the reference's default compression. The C++ side discovers
# the node by announce, establishes a Link, and fetches both. It must retrieve
# and verify the uncompressed page (a multi-packet Resource, msgpack-decoded),
# and the compressed one must FAIL, because microReticulum has no bz2. This is
# the desktop browser's fetch edge reduced to the wire.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_page_fetch.sh
#
# A PASS needs both sides to exit 0 and print SUCCESS.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

PAGE_FILE="$HERE/../desktop/pages/index.mu"

SCENARIO="page-fetch"
PY_SCRIPT="$HERE/python/page_fetch_server.py"
CPP_PROJECT="$HERE/page_fetch_requester"
PY_ARGS="--page-file $PAGE_FILE $*"
: "${TIMEOUT_S:=40}"

# The C++ side reads the same page to compare against; driver.sh launches it in
# an inherited environment, so export it here.
export THICKET_PAGE_FILE="$PAGE_FILE"

source "$HERE/scripts/driver.sh"
