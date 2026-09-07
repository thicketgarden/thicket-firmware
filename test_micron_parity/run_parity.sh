#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Diff our Micron parser's callbacks against NomadNet's own parser, page for
# page and span for span.
#
#   bash test_micron_parity/run_parity.sh
#
# Needs a Python environment with nomadnet installed. Set MICRON_PY to point at
# one, or the script builds a throwaway venv with uv.
#
# Exits non-zero on any divergence, so it can gate a build. A divergence is not
# automatically our bug: the reference is the grammar of record, so the fix is
# either our parser or a documented deviation in MICRON.md, never a silent
# adjustment of the expected output.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK="${TMPDIR:-/tmp}/micron-parity.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

PAGES=("$HERE"/corpus/*.mu)
[[ ${#PAGES[@]} -gt 0 ]] || { echo "[parity] no corpus pages"; exit 1; }

# --- the reference -----------------------------------------------------------
PY="${MICRON_PY:-}"
if [[ -z "$PY" ]]; then
  command -v uv >/dev/null || { echo "[parity] need uv, or set MICRON_PY to a python with nomadnet"; exit 1; }
  uv venv "$WORK/venv" -q || exit 1
  uv pip install -q --python "$WORK/venv/bin/python" nomadnet || exit 1
  PY="$WORK/venv/bin/python"
fi
"$PY" -c 'import nomadnet' 2>/dev/null || { echo "[parity] $PY has no nomadnet"; exit 1; }
REF_VER=$("$PY" -c 'import importlib.metadata as m; print(m.version("nomadnet"))' 2>/dev/null || echo unknown)

"$PY" "$HERE/reference_dump.py" "${PAGES[@]}" > "$WORK/reference.events" || {
  echo "[parity] reference dump failed"; exit 1; }

# --- ours --------------------------------------------------------------------
# Bare compiler, no PlatformIO, no firmware sources. If this ever needs more
# than the parser and a C++17 compiler, the parser has grown a dependency it
# should not have.
c++ -std=c++17 -Wall -Wextra -I "$ROOT/lib/Micron/src" \
    -o "$WORK/ours_dump" "$HERE/ours_dump.cpp" "$ROOT/lib/Micron/src/Micron.cpp" || {
  echo "[parity] our dumper failed to build"; exit 1; }
"$WORK/ours_dump" "${PAGES[@]}" > "$WORK/ours.events" || { echo "[parity] our dump failed"; exit 1; }

# Drop the classes the reference cannot yet report. See reference_dump.py.
grep -v '^SKIP_' "$WORK/ours.events" > "$WORK/ours.cmp"
cp "$WORK/reference.events" "$WORK/reference.cmp"

echo "[parity] nomadnet $REF_VER · ${#PAGES[@]} pages · $(wc -l < "$WORK/reference.cmp" | tr -d ' ') reference events"

if diff -u "$WORK/reference.cmp" "$WORK/ours.cmp" > "$WORK/diff"; then
  echo "[parity] PASS, every compared event matches the reference"
  exit 0
fi
echo "[parity] FAIL, $(grep -c '^[+-][^+-]' "$WORK/diff") differing lines:"
echo
sed -n '1,120p' "$WORK/diff"
echo
echo "[parity] left is NomadNet's parser, right is ours."
exit 1
