#!/usr/bin/env python3
"""Every build path must resolve the same dependency commit.

The failure this exists to stop, from 2026-08-03: microStore was forked so that
microReticulum could call touch() and set_protect_fn(). The root platformio.ini
was updated. Four scenario platformio.ini files in this repo weren't, and four
more inside the pinned microReticulum tree weren't either. Every one of them
built our microReticulum against upstream microStore, where those methods don't
exist, and CI went red twice before the real cause was found.

The second half of the same failure: a git URL with no ref resolves to the
repository's default branch. Our forks keep `master` as a mirror of upstream, so
`.../wet-bulb/microStore.git` with no `#sha` silently fetched a tree with none
of our changes on it. Changing the URL alone did nothing, which cost another
round to notice.

So there are four rules, and the third is the one nothing previously checked:

  R1  every git dependency in the root platformio.ini carries a 40-hex SHA
  R2  every scenario platformio.ini in this repo agrees with the root
  R3  the pinned microReticulum tree's OWN pins agree with the root
  R4  nothing that's pinned to a fork is left on an implicit default branch

Exit codes: 0 pass, 1 a pin disagrees, 2 the check couldn't run. Two is
deliberately distinct: a gate that couldn't run must not read as a gate that
passed.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROOT_INI = REPO / "platformio.ini"

# A git dependency, with or without a ref. The ref group is optional precisely
# so that a missing one can be reported rather than skipped.
DEP = re.compile(
    r"https://github\.com/(?P<owner>[A-Za-z0-9_.-]+)/(?P<lib>[A-Za-z0-9_.-]+)\.git"
    r"(?:#(?P<ref>[^\s\"']+))?"
)

SHA40 = re.compile(r"^[0-9a-f]{40}$")


def fail(msg):
    print(f"[check_pins] FAIL {msg}", file=sys.stderr)


def cannot_run(msg):
    print(f"[check_pins] COULD NOT RUN: {msg}", file=sys.stderr)
    print("[check_pins] This is not a pass. The pins are unverified.", file=sys.stderr)
    sys.exit(2)


def deps_in(path):
    """{lib: (owner, ref_or_None, line_no)} for one ini file."""
    out = {}
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if line.lstrip().startswith(";"):
            continue
        m = DEP.search(line)
        if m:
            out[m.group("lib")] = (m.group("owner"), m.group("ref"), n)
    return out


def main():
    if not ROOT_INI.is_file():
        cannot_run(f"no platformio.ini at {ROOT_INI}")

    root = deps_in(ROOT_INI)
    if not root:
        cannot_run(f"no git dependencies found in {ROOT_INI}")

    bad = 0

    # R1 + R4, the root is authoritative, so it must be unambiguous itself.
    for lib, (owner, ref, n) in sorted(root.items()):
        if ref is None:
            fail(
                f"{ROOT_INI.name}:{n} {owner}/{lib} has no #ref, so it resolves to "
                f"whatever the default branch happens to be. Pin a 40-hex SHA."
            )
            bad += 1
        elif not SHA40.match(ref):
            fail(
                f"{ROOT_INI.name}:{n} {owner}/{lib} is pinned to '{ref}', which is "
                f"not a 40-hex SHA. Branches and tags move; the build stops being "
                f"reproducible and the recorded flash figures stop meaning anything."
            )
            bad += 1

    # R2, this repo's scenario projects carry their own copies of the pins.
    for ini in sorted(REPO.glob("test_interop/*/platformio.ini")):
        for lib, (owner, ref, n) in sorted(deps_in(ini).items()):
            if lib not in root:
                continue
            r_owner, r_ref, _ = root[lib]
            if (owner, ref) != (r_owner, r_ref):
                rel = ini.relative_to(REPO)
                fail(
                    f"{rel}:{n} {lib} is {owner}@{ref or '<no ref>'} but the root "
                    f"says {r_owner}@{r_ref}. This scenario is testing a stack we "
                    f"do not ship."
                )
                bad += 1

    # R3, the pins INSIDE the pinned dependency. Nothing checked this before,
    # and it's what actually broke: microReticulum's own scenarios pin their
    # own microStore, and don't inherit ours.
    checked_inner = False
    for libdeps in sorted(REPO.glob(".pio/libdeps/*/microReticulum")):
        checked_inner = True
        for ini in sorted(libdeps.glob("test_interop/*/platformio.ini")):
            for lib, (owner, ref, n) in sorted(deps_in(ini).items()):
                if lib not in root:
                    continue
                r_owner, r_ref, _ = root[lib]
                if (owner, ref) != (r_owner, r_ref):
                    fail(
                        f"inside pinned microReticulum: "
                        f"test_interop/{ini.parent.name}/platformio.ini:{n} {lib} is "
                        f"{owner}@{ref or '<no ref>'} but the root says "
                        f"{r_owner}@{r_ref}. The interop job builds these, and it "
                        f"will fail to compile if our tree needs a fork API."
                    )
                    bad += 1
        break

    if not checked_inner:
        print(
            "[check_pins] note: microReticulum is not fetched into .pio/libdeps, "
            "so rule 3 (pins inside the pinned dependency) was not checked. Run "
            "`pio pkg install -e wiscore_rak4631` first for full coverage.",
            file=sys.stderr,
        )

    if bad:
        print(
            f"\n[check_pins] {bad} pin problem(s). Every build path has to resolve "
            f"the same commit, or CI tests a stack nobody ships.",
            file=sys.stderr,
        )
        return 1

    scope = "root + scenarios" + (" + inside the dependency" if checked_inner else "")
    print(f"[check_pins] OK, {len(root)} dependencies agree across {scope}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
