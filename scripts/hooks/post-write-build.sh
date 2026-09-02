#!/usr/bin/env bash
# PostToolUse hook (Write|Edit): if a source file changed, both build
# environments must still link. Exit 2 = feedback to the agent.
#
# This lives in a file rather than inline in .claude/settings.json so that a
# failure to LOCATE the gate reads differently from a failure OF the gate. The
# inline version did `cd "$CLAUDE_PROJECT_DIR" && pio run || echo "build
# FAILED"`, so a stale or empty project dir reported a build break that hadn't
# happened, which sends whoever reads it chasing the wrong thing.
set -uo pipefail

cd "$(dirname "$0")/../.." || {
	echo "build gate: could not reach the repo root from $0. The gate did NOT run." >&2
	exit 2
}

path=$(/usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))' 2>/dev/null || true)

case "$path" in
	*.cpp|*.h|*.c|*.ino|*platformio.ini) ;;
	*) exit 0 ;;
esac

if ! command -v pio >/dev/null 2>&1; then
	echo "build gate: pio not on PATH. The gate did NOT run, so this change is unverified." >&2
	exit 2
fi

if ! pio run -e wiscore_rak4631 -e wiscore_rak4631-noble >/dev/null 2>&1; then
	echo "build FAILED - both envs must pass. The no-BLE env is the flash fallback; if it rots it is not a fallback." >&2
	exit 2
fi
