#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Render the UI screens through the real display driver and open them.
# Nothing here touches hardware.
#
#   scripts/screens.sh          render, convert, open
#   scripts/screens.sh --no-open

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

echo "[screens] rendering through SharpLcd + VirtualPanel..."
if ! pio test -e native -f test_virtual_panel > /tmp/screens.log 2>&1; then
  echo "[screens] render FAILED:"
  grep -E 'error:|FAILED' /tmp/screens.log | head -20
  exit 1
fi

shopt -s nullglob
pbms=(screens/*.pbm)
if [[ ${#pbms[@]} -eq 0 ]]; then
  echo "[screens] no screens produced - is a render test writing to screens/ ?"
  exit 1
fi

for f in "${pbms[@]}"; do
  python3 scripts/pbm2png.py "$f" "${f%.pbm}.png" || exit 1
done

echo "[screens] ${#pbms[@]} screen(s) in $(pwd)/screens"
[[ "${1:-}" == "--no-open" ]] && exit 0
command -v open >/dev/null && open screens/*.png
