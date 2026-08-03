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
  "lxmf-inbound|lxmf_inbound_receiver|run_lxmf_inbound.sh"
  "identity-vectors|identity_vectors|run_identity_vectors.sh"
  "link-inbound|link_inbound_responder|run_link_inbound.sh"
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
echo "=== summary ==="
if [[ ${#FAILED[@]} -eq 0 ]]; then
  echo "[run_all] all ${#SCENARIOS[@]} scenarios PASS"
  exit 0
fi
echo "[run_all] ${#FAILED[@]} of ${#SCENARIOS[@]} scenarios FAILED: ${FAILED[*]}"
exit 1
