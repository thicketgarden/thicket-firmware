#!/usr/bin/env bash
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared two-process driver for the Thicket interop scenarios.
#
# Source this from a run_*.sh after setting:
#   SCENARIO     human-readable name
#   PY_SCRIPT    absolute path to the Python side
#   PY_ARGS      (optional) extra args for the Python side
#   CPP_PROJECT  absolute path to the scenario's PlatformIO project dir
#   TIMEOUT_S    (optional, default 40) per-side timeout
#
# TWO-C++ MODE. Set PEER_CPP_ROLE instead of PY_SCRIPT and the first side is a
# second copy of the same binary rather than a Python process, with THICKET_ROLE
# set to that value; the C++ side then gets CPP_ROLE.
#
# Added for the two-node scenario, where BOTH ends are ours. That contract had
# already been stretched twice by having the Python side spawn a child, and
# stretching it a third time would have hidden the thing being tested inside a
# process tree. Everything else is unchanged: both sides must exit 0 AND print
# SUCCESS, which is exactly the guarantee this scenario needs, because a node
# that stays silent because it crashed looks identical to one with nothing to
# say.
#
# A scenario PASSES iff BOTH sides exit 0 AND BOTH logs contain "SUCCESS".
# Requiring an explicit SUCCESS token in addition to the exit code is
# deliberate: a process that dies before it asserts anything exits non-zero,
# but a process whose assertions were accidentally compiled out could still
# exit 0. Both sides have to say so.

set -u

: "${TIMEOUT_S:=40}"
: "${PY_ARGS:=}"

CPP_BIN="$CPP_PROJECT/.pio/build/native17/program"

if [[ ! -x "$CPP_BIN" ]]; then
  echo "[driver] $SCENARIO: C++ binary not found at $CPP_BIN" >&2
  echo "[driver] build it with:" >&2
  echo "[driver]   bash test_interop/scripts/fetch_deps.sh" >&2
  echo "[driver]   cd $CPP_PROJECT && pio run -e native17" >&2
  exit 1
fi

if [[ -z "${PEER_CPP_ROLE:-}" ]] && ! command -v python3 >/dev/null 2>&1; then
  echo "[driver] $SCENARIO: no python3 on PATH" >&2
  exit 1
fi

WORKDIR="$(mktemp -d -t thicket_interop.XXXXXX)"
cleanup() {
  [[ -n "${PY_PID:-}" ]]  && kill -0 "$PY_PID"  2>/dev/null && kill "$PY_PID"  2>/dev/null
  [[ -n "${CPP_PID:-}" ]] && kill -0 "$CPP_PID" 2>/dev/null && kill "$CPP_PID" 2>/dev/null
  rm -rf "$WORKDIR"
  return 0
}
trap cleanup EXIT

PY_LOG="$WORKDIR/python.log"
CPP_LOG="$WORKDIR/cpp.log"

if [[ -n "${PEER_CPP_ROLE:-}" ]]; then
  # Two-C++ mode. Same scratch-directory treatment as the second side below:
  # microStore's POSIX adapter ignores its basepath and works at the process
  # CWD, so two nodes sharing one directory would share an identity store and
  # silently stop being two nodes.
  PEER_RUNDIR="$WORKDIR/peer"
  mkdir -p "$PEER_RUNDIR"
  echo "[driver] $SCENARIO: launching C++ peer (role=$PEER_CPP_ROLE)"
  ( cd "$PEER_RUNDIR" && THICKET_INTEROP_TIMEOUT_S="$TIMEOUT_S" \
      THICKET_ROLE="$PEER_CPP_ROLE" exec "$CPP_BIN" ) >"$PY_LOG" 2>&1 &
  PY_PID=$!
else
  echo "[driver] $SCENARIO: launching Python side"
  # PY_RNS is honoured for parity with the upstream drivers; empty (the normal
  # case here) means "use whatever RNS is importable from PATH's python3".
  PYTHONPATH="${PY_RNS:-}" python3 "$PY_SCRIPT" --timeout "$TIMEOUT_S" $PY_ARGS \
    >"$PY_LOG" 2>&1 &
  PY_PID=$!
fi

sleep 2

echo "[driver] $SCENARIO: launching C++ side"
# Run it in a scratch directory. microStore's POSIX adapter ignores its
# basepath and does every file operation at the process CWD, so a binary
# started from the repo root drops transport_identity, known_store/,
# path_store/ and hashlist_store/ into the working tree. Also means each run
# starts with no inherited identity or path table, which is what a scenario
# wants.
CPP_RUNDIR="$WORKDIR/cpp"
mkdir -p "$CPP_RUNDIR"
# Keep the C++ side's own deadline inside the watchdog's, so a failure ends in
# its TIMEOUT diagnostic rather than an uninformative SIGTERM.
( cd "$CPP_RUNDIR" && THICKET_INTEROP_TIMEOUT_S="$TIMEOUT_S" \
    THICKET_ROLE="${CPP_ROLE:-}" exec "$CPP_BIN" ) \
  >"$CPP_LOG" 2>&1 &
CPP_PID=$!

DEADLINE=$(( $(date +%s) + TIMEOUT_S + 15 ))
while true; do
  if ! kill -0 "$PY_PID" 2>/dev/null && ! kill -0 "$CPP_PID" 2>/dev/null; then
    break
  fi
  if (( $(date +%s) >= DEADLINE )); then
    echo "[driver] $SCENARIO: watchdog timeout"
    break
  fi
  sleep 0.5
done

kill "$PY_PID"  2>/dev/null
kill "$CPP_PID" 2>/dev/null
wait "$PY_PID"  2>/dev/null; PY_RC=$?
wait "$CPP_PID" 2>/dev/null; CPP_RC=$?

echo "--- ${PEER_CPP_ROLE:+c++ peer ($PEER_CPP_ROLE)}${PEER_CPP_ROLE:-python side} ---"
cat "$PY_LOG"
echo "--- c++ side ---"
cat "$CPP_LOG"
echo "--- summary ---"
echo "[driver] $SCENARIO: python exit $PY_RC, c++ exit $CPP_RC"

FAILED=0
[[ $PY_RC  -eq 0 ]] || { echo "[driver] python side exited $PY_RC"; FAILED=1; }
[[ $CPP_RC -eq 0 ]] || { echo "[driver] c++ side exited $CPP_RC";   FAILED=1; }
grep -q "SUCCESS" "$PY_LOG"  || { echo "[driver] python side never printed SUCCESS"; FAILED=1; }
grep -q "SUCCESS" "$CPP_LOG" || { echo "[driver] c++ side never printed SUCCESS";    FAILED=1; }

if [[ $FAILED -eq 0 ]]; then
  echo "[driver] PASS ($SCENARIO)"
  exit 0
fi
echo "[driver] FAIL ($SCENARIO)"
exit 1
