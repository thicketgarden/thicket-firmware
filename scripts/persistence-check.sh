#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Does the identity survive a power cycle?
#
# This is the last milestone clause a demo cannot fake. The device must come
# back as the SAME address after power is removed, not merely after a reset,
# and the peer has to be able to reach it on that address afterwards.
#
# Two phases with a physical act between them, because a power cycle is not
# something software can honestly perform on itself:
#
#   scripts/persistence-check.sh arm      # flash, boot, record the identity
#   ---- pull the USB cable, count to five, plug it back in ----
#   scripts/persistence-check.sh verify   # boot again, compare
#
# `arm` flashes by default. Pass --no-flash to re-arm against firmware that is
# already on the board.
#
# Requires the RAK15001 external flash to be fitted. Without it the firmware
# reports an ephemeral identity and this script says so rather than failing in
# a way that looks like a persistence bug.

set -uo pipefail
cd "$(dirname "$0")/.."

ENV="${ENV:-wiscore_rak4631}"
STATE="${STATE:-.persistence-check}"
SECONDS_BOOT="${SECONDS_BOOT:-25}"

phase="${1:-}"
shift || true
no_flash=0
for a in "$@"; do [ "$a" = "--no-flash" ] && no_flash=1; done

die() { echo "[persistence] $*" >&2; exit 1; }

# The identity hash as the firmware prints it at boot.
extract_identity() {
	grep -oE "identity hash[^0-9a-f]*([0-9a-f]{16,})" "$1" | grep -oE "[0-9a-f]{16,}" | head -1
}

capture_boot() {
	local out="$1"
	if [ "$no_flash" = "1" ] || [ "$phase" = "verify" ]; then
		python3 scripts/board.py capture --seconds "$SECONDS_BOOT" > "$out" 2>&1
	else
		python3 scripts/board.py flash "$ENV" --seconds "$SECONDS_BOOT" > "$out" 2>&1
	fi
}

report_ephemeral() {
	cat >&2 <<'MSG'
[persistence] The firmware reports an EPHEMERAL identity, which means no
[persistence] filesystem was registered — the RAK15001 is not fitted, or the
[persistence] build was a -noflash / -internalfs one. That is not a
[persistence] persistence failure; there is nothing to persist to. Fit the
[persistence] flash and build the default env.
MSG
	exit 2
}

case "$phase" in
arm)
	log="${STATE}.arm.log"
	echo "[persistence] phase 1: $( [ "$no_flash" = 1 ] && echo capture || echo "flash $ENV" ), then read the identity"
	capture_boot "$log" || die "board.py failed; see $log"
	grep -qi "identity is EPHEMERAL" "$log" && report_ephemeral
	id="$(extract_identity "$log")"
	[ -n "$id" ] || die "no identity hash in the boot log; see $log"
	printf '%s\n' "$id" > "${STATE}.id"
	echo "[persistence] identity now: $id"
	echo "[persistence] recorded. NOW PULL THE USB CABLE, wait five seconds,"
	echo "[persistence] plug it back in, then run:"
	echo "[persistence]   scripts/persistence-check.sh verify"
	;;
verify)
	[ -f "${STATE}.id" ] || die "nothing armed — run 'arm' first"
	before="$(cat "${STATE}.id")"
	log="${STATE}.verify.log"
	echo "[persistence] phase 2: reading the identity after the power cycle"
	capture_boot "$log" || die "board.py failed; see $log"
	grep -qi "identity is EPHEMERAL" "$log" && report_ephemeral
	after="$(extract_identity "$log")"
	[ -n "$after" ] || die "no identity hash in the boot log; see $log"

	echo "[persistence] before: $before"
	echo "[persistence] after:  $after"
	if [ "$before" = "$after" ]; then
		if grep -qi "identity restored across reboot" "$log"; then
			echo "[persistence] PASS — same address, and the firmware says it was"
			echo "[persistence] restored from external flash rather than reminted."
			exit 0
		fi
		echo "[persistence] SUSPECT — the hash matches but the firmware did not" >&2
		echo "[persistence] report restoring it. Read $log before believing this." >&2
		exit 1
	fi
	echo "[persistence] FAIL — the address changed across the power cycle." >&2
	echo "[persistence] The device is a different peer to anyone who knew it." >&2
	exit 1
	;;
*)
	sed -n '4,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
