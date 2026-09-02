#!/usr/bin/env bash
# PreToolUse hook (Bash): refuse `git push` until the fast checks pass.
#
# Written 2026-08-03, after a push went out with a pin that disagreed across
# build paths and a unit test that asserted unspecified behaviour. Both were
# caught by CI rather than before the push, and the second attempt to fix them
# was also pushed unverified. The information needed was one command away both
# times.
#
# So this is a control rather than a resolution to be careful. It runs only the
# fast checks: pins, public-text shorthand, then native tests, because a gate
# slow enough to be
# resented gets bypassed, and a bypassed gate is worse than none: it still looks
# like coverage. The interop suites take minutes and stay in CI.
#
# Exit 2 = block the tool call and tell the agent why.
# Exit 0 = allow.
#
# As with the build gate: a failure to LOCATE this script must read differently
# from a failure OF it. Silence here would mean an unverified push looked
# exactly like a verified one.
set -uo pipefail

cd "$(dirname "$0")/../.." || {
	echo "push gate: could not reach the repo root from $0. The gate did NOT run." >&2
	exit 2
}

cmd=$(/usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("command",""))' 2>/dev/null || true)

# Only intercept a push. Everything else goes straight through.
case "$cmd" in
	*"git push"*) ;;
	*) exit 0 ;;
esac

echo "push gate: checking pins, public text and native tests." >&2

if ! command -v python3 >/dev/null 2>&1; then
	echo "push gate: python3 not on PATH. The gate did NOT run, so this push is unverified." >&2
	exit 2
fi

if ! out=$(python3 scripts/check_pins.py 2>&1); then
	echo "push gate: BLOCKED, dependency pins disagree across build paths." >&2
	echo "$out" >&2
	echo "" >&2
	echo "Every build path has to resolve the same commit. Fix the pins, then push." >&2
	exit 2
fi

# Private-repo shorthand, in the content AND in the messages about to go out.
# Added 2026-08-04 after the rule was swept for three times in one day and
# broken three times in the same day, ending in fifteen rewritten commit
# messages. Messages are checked because they're the half that can't be fixed
# afterwards without rewriting published history.
if ! out=$(python3 scripts/check_public_text.py --outgoing 2>&1); then
	echo "push gate: BLOCKED, a commit message about to be published carries" >&2
	echo "           private-repo shorthand." >&2
	echo "$out" >&2
	echo "" >&2
	echo "These cannot be fixed after the push without rewriting history." >&2
	exit 2
fi

if ! out=$(python3 scripts/check_public_text.py --all 2>&1); then
	echo "push gate: BLOCKED, tracked content carries private-repo shorthand." >&2
	echo "$out" >&2
	exit 2
fi

if ! command -v pio >/dev/null 2>&1; then
	echo "push gate: pio not on PATH. Native tests did NOT run, so this push is unverified." >&2
	exit 2
fi

if ! out=$(pio test -e native 2>&1); then
	echo "push gate: BLOCKED, native tests fail." >&2
	echo "$out" | tail -25 >&2
	exit 2
fi

echo "push gate: pins agree, no private-repo shorthand, native tests pass." >&2
echo "note: the interop suites are not run here. If a pin moved, run them." >&2
echo "      bash test_interop/run_all.sh, before relying on this push." >&2
exit 0
