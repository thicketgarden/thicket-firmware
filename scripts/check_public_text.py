#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Refuse to let private-repo shorthand reach this public repository.
#
# This repo is public. The private one keeps a register of numbered decisions
# (A-numbers), tasks (T-numbers), open questions (Q-numbers) and milestones
# (M1-M5). None of it means anything to a stranger reading this code, and a
# reference to the private repo's paths tells them about a repository they
# cannot open.
#
# WHY THIS EXISTS RATHER THAN A HABIT OF CHECKING
#
# The rule is old and was known. On 2026-08-04 it was swept for three separate
# times in one day and violated three separate times in the same day, because
# sweeping is something a person remembers to do and this is not. Fifteen commit
# messages going back three days had to be rewritten, several of which named the
# private repo outright. The information needed to prevent every one of them was
# available at the moment of writing.
#
# So: a control, run by the push gate, over BOTH the staged content and the
# commit messages about to go out. Messages matter as much as content and are
# harder to fix afterwards -- fixing them means rewriting published history.
#
# Usage:
#   python3 scripts/check_public_text.py --staged      # content about to be committed
#   python3 scripts/check_public_text.py --outgoing    # commits about to be pushed
#   python3 scripts/check_public_text.py --all         # every tracked file
#   python3 scripts/check_public_text.py FILE...

import re
import subprocess
import sys

# Ordered: the first match wins, so the specific private-repo names are reported
# as themselves rather than as a generic token.
PATTERNS = [
    (re.compile(r"\bthicket-hq\b"), "names the private repository"),
    (re.compile(r"\btasks/T\d+\b"), "names a path inside the private repository"),
    (re.compile(r"(?<![\w-])in hq\b"), "refers to the private repository"),
    (re.compile(r"\bA\d{1,2}'?(?![\w.-])"), "register (A-number) reference"),
    (re.compile(r"\bT\d{1,3}(?![\w.-])"), "task (T-number) reference"),
    (re.compile(r"\bQ\d{1,2}(?![\w.-])"), "open-question (Q-number) reference"),
    (re.compile(r"\bM[1-5](?![\w.-])"), "milestone reference"),
]

# Things that look like shorthand and are not. Kept narrow and explained,
# because a permissive allow-list is how a control stops controlling.
ALLOW = [
    re.compile(r"\bPIN_A\d\b"),                 # variant.h analogue pin names
    re.compile(r"\bA[0-7]\s*=\s*PIN_A"),        # ditto, the definitions
    re.compile(r"Cortex-M[0-7]"),               # the core, not a milestone
    re.compile(r"\bM4 at \d+ MHz"),             # ditto, in prose
    re.compile(r"\bT\d+ms\b", re.I),            # timing symbols
    re.compile(r"MIL-[A-Z]-\d+"),               # standards
    re.compile(r"\bQ[1-4] 20\d\d"),             # calendar quarters
    re.compile(r"\bA\d{1,2}(?:\.\d+)+"),        # dotted version-like tokens
]

SKIP_PREFIXES = (
    ".pio/", ".git/", "test_interop/.deps/", "boards/", "variants/",
)
SKIP_SUFFIXES = (".json", ".map", ".bin", ".hex", ".uf2", ".zip", ".png")

# The only file-level exemption, and it has to exist: this file's patterns and
# its worked example are made of the very tokens it looks for. Any other
# exemption is a hole -- reword the text instead of adding one here.
SELF = "scripts/check_public_text.py"


def allowed(line, start, end):
    for a in ALLOW:
        for m in a.finditer(line):
            if m.start() <= start and m.end() >= end:
                return True
    return False


def scan(text, label, out):
    for n, line in enumerate(text.splitlines(), 1):
        for pat, why in PATTERNS:
            for m in pat.finditer(line):
                if allowed(line, m.start(), m.end()):
                    continue
                out.append((label, n, m.group(0), why, line.strip()[:88]))
                break
    return out


def tracked_files():
    r = subprocess.run(["git", "ls-files"], capture_output=True, text=True)
    return [f for f in r.stdout.splitlines()
            if not f.startswith(SKIP_PREFIXES) and not f.endswith(SKIP_SUFFIXES)
            and f != SELF]


def staged_files():
    r = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
                       capture_output=True, text=True)
    return [f for f in r.stdout.splitlines()
            if not f.startswith(SKIP_PREFIXES) and not f.endswith(SKIP_SUFFIXES)
            and f != SELF]


def outgoing_messages():
    """Commit messages that a push would publish.

    Falls back to the whole branch when there is no upstream yet, because a
    first push publishes everything and that is exactly when it matters.
    """
    rng = subprocess.run(["git", "rev-list", "@{u}..HEAD"],
                         capture_output=True, text=True)
    if rng.returncode != 0:
        rng = subprocess.run(["git", "rev-list", "HEAD"],
                             capture_output=True, text=True)
    out = []
    for h in rng.stdout.split():
        msg = subprocess.run(["git", "log", "-1", "--format=%s%n%b", h],
                             capture_output=True, text=True).stdout
        out.append((h[:9], msg))
    return out


def main():
    args = sys.argv[1:]
    findings = []

    if "--outgoing" in args:
        for h, msg in outgoing_messages():
            scan(msg, f"commit {h}", findings)
    elif "--staged" in args:
        for f in staged_files():
            try:
                scan(open(f, errors="replace").read(), f, findings)
            except (OSError, IsADirectoryError):
                pass
    elif "--all" in args:
        for f in tracked_files():
            try:
                scan(open(f, errors="replace").read(), f, findings)
            except (OSError, IsADirectoryError):
                pass
    else:
        for f in args:
            scan(open(f, errors="replace").read(), f, findings)

    if not findings:
        print("check_public_text: OK — no private-repo shorthand.")
        return 0

    print(f"check_public_text: {len(findings)} reference(s) that mean nothing "
          f"to a reader of this repository:\n", file=sys.stderr)
    for label, n, tok, why, line in findings[:40]:
        print(f"  {label}:{n}: '{tok}' — {why}", file=sys.stderr)
        print(f"      {line}", file=sys.stderr)
    if len(findings) > 40:
        print(f"  … and {len(findings) - 40} more", file=sys.stderr)
    print("\nTranslate to a plain sentence rather than deleting the token: not\n"
          "\"blocked on A11\" but \"waiting on a measured battery figure\".\n"
          "A message with a hole in it is worse than one with jargon.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
