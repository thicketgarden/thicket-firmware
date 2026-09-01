#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bring up an RNode on this machine as a reference peer for the board.
#
# Radio settings are taken from lib/LoRaInterface/src/LoRaInterface.h and must
# match it exactly. Being one channel width off leaves both passbands barely
# overlapping and the two ends mutually deaf — which presents as a dead radio
# rather than as a misconfiguration, and has cost a bring-up session before.
#
#   scripts/bench-peer.sh detect     # find the RNode and show its config
#   scripts/bench-peer.sh configure  # write the interface into ~/.reticulum
#   scripts/bench-peer.sh up         # run rnsd in the foreground
#   scripts/bench-peer.sh status     # rnstatus

set -uo pipefail

FREQ_HZ=914875000      # 914.875 MHz
BANDWIDTH_HZ=125000    # 125 kHz
SPREADING=8
CODING=5
TXPOWER=17             # dBm
IFACE_NAME="Thicket RNode"
CONFIG="$HOME/.reticulum/config"
BIN="$HOME/.local/bin"

die() { echo "[peer] $*" >&2; exit 1; }

find_rnode() {
	# The RNode enumerates as a USB serial device. Exclude the board itself if
	# both are plugged in: the caller can override with RNODE_PORT.
	if [ -n "${RNODE_PORT:-}" ]; then printf '%s\n' "$RNODE_PORT"; return 0; fi
	ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -5
}

case "${1:-}" in
detect)
	ports="$(find_rnode)"
	[ -n "$ports" ] || die "no USB serial devices found — is the RNode plugged in?"
	echo "[peer] candidate ports:"
	printf '  %s\n' $ports
	echo "[peer] probing each (set RNODE_PORT to skip this):"
	for p in $ports; do
		echo "  --- $p ---"
		"$BIN/rnodeconf" "$p" -i 2>&1 | sed 's/^/      /' | head -12
	done
	;;
configure)
	port="$(find_rnode | head -1)"
	[ -n "${RNODE_PORT:-}" ] && port="$RNODE_PORT"
	[ -n "$port" ] || die "no port; plug in the RNode or set RNODE_PORT"
	mkdir -p "$(dirname "$CONFIG")"
	[ -f "$CONFIG" ] && cp "$CONFIG" "$CONFIG.bak.$(date +%Y%m%d%H%M%S)"

	if grep -q "\[\[$IFACE_NAME\]\]" "$CONFIG" 2>/dev/null; then
		echo "[peer] '$IFACE_NAME' already in $CONFIG — leaving it alone."
		echo "[peer] Edit it by hand or remove the block and re-run."
		exit 0
	fi

	cat >> "$CONFIG" <<EOF

  # Added by thicket scripts/bench-peer.sh. Settings mirror
  # lib/LoRaInterface/src/LoRaInterface.h — change both or neither.
  [[$IFACE_NAME]]
    type = RNodeInterface
    enabled = Yes
    port = $port
    frequency = $FREQ_HZ
    bandwidth = $BANDWIDTH_HZ
    txpower = $TXPOWER
    spreadingfactor = $SPREADING
    codingrate = $CODING
EOF
	echo "[peer] wrote '$IFACE_NAME' on $port into $CONFIG"
	echo "[peer] 914.875 MHz · 125 kHz · SF$SPREADING · CR4/$CODING · ${TXPOWER} dBm"
	;;
up)
	command -v "$BIN/rnsd" >/dev/null || die "rnsd not found at $BIN"
	echo "[peer] starting rnsd (ctrl-C to stop)"
	exec "$BIN/rnsd" -v
	;;
status)
	exec "$BIN/rnstatus"
	;;
*)
	sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
