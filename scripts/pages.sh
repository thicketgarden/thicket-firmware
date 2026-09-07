#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Render Micron pages to 400x240 1-bit PNGs through the real driver and font.
#
#   scripts/pages.sh <corpus-dir> [leading] [flat]
#
# Built with a bare compiler, not through PlatformIO: the render loop is
# look-fix-look and a 75-second test cycle is not a loop. This is ~0.5 s.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
CORPUS="${1:?usage: pages.sh <corpus-dir> [leading] [flat]}"
LEAD="${2:-0}"; FLAT="${3:-}"
MC=.pio/libdeps/native/micron-cpp/src
[[ -d "$MC" ]] || { echo "[pages] micron-cpp not fetched; run: pio test -e native"; exit 1; }

# path -> mode, for files the manifest marks 100755.
MODES=""
if [[ -f "$CORPUS/MANIFEST.tsv" ]]; then
  MODES=$(awk -F'\t' 'NR>1 && $6=="100755" {print $1"\t"}' "$CORPUS/MANIFEST.tsv")
fi

mkdir -p pages
c++ -std=c++17 -O1 -Wall -Wextra -I lib/MicronRender/src -I lib/SharpLcd/src -I "$MC" \
    -o /tmp/render_page scripts/render_page.cpp lib/MicronRender/src/MicronRender.cpp \
    lib/SharpLcd/src/SharpLcd.cpp lib/SharpLcd/src/VirtualPanel.cpp "$MC/Micron.cpp" || exit 1

rm -f pages/*.pbm pages/*.png
n=0
while IFS= read -r p; do
  # A page with the executable bit is a PROGRAM whose output is Micron, so its
  # source is not layout input. The MANIFEST is the authority: a shebang is not
  # the marker (some static pages open with one, and #!c=3600 is a NomadNet
  # cache directive, not a shebang), and a fetched copy has lost its mode.
  rel="${p#"$CORPUS"/}"
  if [[ -n "$MODES" ]] && grep -qF "$(printf '%s\t' "$rel")" <<< "$MODES"; then
    echo "  skip (executable page): $rel"; continue
  fi
  slug=$(basename "$p" .mu | tr ' /' '__')
  /tmp/render_page "$p" "pages/$slug" "$LEAD" "$FLAT" && n=$((n+1))
done < <(find "$CORPUS" -name '*.mu' | sort)

for f in pages/*.pbm; do python3 scripts/pbm2png.py "$f" "${f%.pbm}.png" >/dev/null || exit 1; done
echo "[pages] $n page(s) -> $(pwd)/pages"
