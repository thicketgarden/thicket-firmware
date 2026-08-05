#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run every Thicket interop scenario. One command, CI-shaped: exits
# non-zero if any scenario fails.
#
#   PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_all.sh
#
# Set BUILD=0 to skip the build step and run whatever is already built.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${BUILD:=1}"

SCENARIOS=(
  "cold-inbound|cold_inbound_receiver|run_cold_inbound.sh"
  # Reuses cold_inbound_receiver's binary deliberately -- same leaf, different
  # topology -- so the second build here is a no-op.
  "multihop-inbound|cold_inbound_receiver|run_multihop_inbound.sh"
  "lxmf-inbound|lxmf_inbound_receiver|run_lxmf_inbound.sh"
  "identity-vectors|identity_vectors|run_identity_vectors.sh"
  "link-inbound|link_inbound_responder|run_link_inbound.sh"
  # The only scenario where we are the router rather than the leaf. Slowest of
  # the six: three processes, and a path has to be learned before anything can
  # be sent.
  "transport-forward|transport_forwarder|run_transport_forward.sh"
)

echo "=== fetching pinned dependencies ==="
bash "$HERE/scripts/fetch_deps.sh" || exit 1

if [[ "$BUILD" == "1" ]]; then
  for entry in "${SCENARIOS[@]}"; do
    IFS='|' read -r name project _ <<< "$entry"
    echo "=== building $name ==="
    ( cd "$HERE/$project" && pio run -e native17 ) >/dev/null 2>&1 || {
      echo "[run_all] BUILD FAILED for $name; rerun the build to see why:"
      echo "[run_all]   cd $HERE/$project && pio run -e native17"
      exit 1
    }
  done
fi

FAILED=()
for entry in "${SCENARIOS[@]}"; do
  IFS='|' read -r name _ script <<< "$entry"
  echo
  echo "=== running $name ==="
  if bash "$HERE/$script"; then
    echo "[run_all] $name PASS"
  else
    echo "[run_all] $name FAIL"
    FAILED+=("$name")
  fi
done

echo
# The versions the result is about. A28' requires that a pin bump invalidates
# the interop claim until the suite is re-run AND the version it passed against
# is recorded -- and until 2026-08-04 nothing recorded it, so a green run was a
# claim about unnamed code. Printing it here means the record cannot be
# forgotten separately from the run.
pin_of() { grep -oE "$1\.git#[a-f0-9]{40}" "$HERE/../platformio.ini" | head -1 | cut -d'#' -f2; }
RNS_VER=$(python3 -c 'import RNS; print(RNS.__version__)' 2>/dev/null || echo "unknown")
LXMF_VER=$(python3 -c 'import LXMF; print(LXMF.__version__)' 2>/dev/null || echo "unknown")

echo
echo "=== versions under test ==="
echo "[run_all] microReticulum $(pin_of microReticulum)"
echo "[run_all] microStore     $(pin_of microStore)"
echo "[run_all] microLXMF      $(pin_of microLXMF)"
echo "[run_all] python rns     $RNS_VER"
echo "[run_all] python lxmf    $LXMF_VER"

echo "=== summary ==="
if [[ ${#FAILED[@]} -eq 0 ]]; then
  echo "[run_all] all ${#SCENARIOS[@]} scenarios PASS"
  exit 0
fi
echo "[run_all] ${#FAILED[@]} of ${#SCENARIOS[@]} scenarios FAILED: ${FAILED[*]}"
exit 1
