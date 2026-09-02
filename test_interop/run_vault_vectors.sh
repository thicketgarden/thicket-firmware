#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scenario: IDENTITY VAULT.
#
# The identity is the device. Sealing it under a code is only worth doing if
# opening it returns the same identity, and if a wrong code returns nothing at
# all rather than bytes that merely look like a key.
#
# The negative cases carry the weight. A round trip proves the writer and the
# reader agree with each other; it cannot prove that a wrong code fails. A
# vault that opened for any code would still print "identity restored" at boot,
# which is the failure nobody would catch.
#
#   bash test_interop/run_vault_vectors.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$HERE/vault_vectors"
BIN="$PROJECT/.pio/build/native17/program"

if [ ! -x "$BIN" ]; then
	echo "[vault] building" >&2
	( cd "$PROJECT" && pio run -e native17 ) >/dev/null 2>&1 || {
		echo "[vault] build failed; rerun to see why:" >&2
		echo "[vault]   cd $PROJECT && pio run -e native17" >&2
		exit 1
	}
fi

cd "$PROJECT" && "$BIN"
