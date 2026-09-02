#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# GPL-3.0-or-later.
#
# Build-time assertion: nothing in this firmware disables the debug port.
#
# WHY THIS IS A SCRIPT AND NOT A COMMENT
#
# On nRF52840 build codes Fxx and later, access port protection is enabled by
# default and takes TWO independent actions to lift (Product Specification,
# "Debug and trace"):
#
#   1. UICR.APPROTECT programmed to HwDisabled  (hardware part)
#   2. firmware writing APPROTECT.DISABLE = SwDisable  (software part)
#
# The 2020 LimitedResults DEC1 voltage glitch defeats (1). It can't perform
# (2). So the debug port on our device stays shut for exactly one reason:
# **no code here performs the software write.**
#
# That's a property of an absence, and absences rot silently. A debug build, a
# bump to MDK >= 8.45.0 (whose startup code performs the write by default so
# developers can attach a debugger), or a differently-vendored system file all
# remove the protection with no error and no diff anyone would notice. If
# anything is ever held in internal flash on the strength of this - a key
# derivation "pepper" for message encryption is the case we have in mind - then
# this check is what makes that safe rather than hopeful.
#
# Run as a PlatformIO pre: script. Fails the build.

import os
import re
import sys

Import("env")  # noqa: F821  (injected by SCons/PlatformIO)

# Tokens that would only appear if someone made the software-disable reachable.
# The BSP's nrf52840.h defines UICR->APPROTECT and nothing else, so today none
# of these exist anywhere - that's the state being defended.
FORBIDDEN = [
    (r"\bNRF_APPROTECT\b",        "the APPROTECT peripheral (software disable)"),
    (r"APPROTECT\s*->\s*DISABLE", "a write to APPROTECT.DISABLE"),
    (r"\bAPPROTECT_DISABLE\b",    "the APPROTECT_DISABLE symbol"),
    (r"\bSwDisable\b",            "the SwDisable value"),
    (r"\bENABLE_APPROTECT\b",     "the MDK ENABLE_APPROTECT flag"),
    # Any assignment to UICR.APPROTECT. Reading it's fine and we do, in the
    # boot diagnostic; writing it's programming the hardware half.
    (r"APPROTECT\s*=(?!=)",       "an assignment to UICR.APPROTECT"),
]

SCAN_EXT = (".c", ".cpp", ".h", ".hpp", ".S", ".s", ".ini")

_BLOCK = re.compile(r"/\*.*?\*/", re.S)
_LINE = re.compile(r"//[^\n]*|;[^\n]*")   # ';' covers .ini comments


def strip_comments(text):
    """Blank out comments, preserving line numbering.

    Without this the check fires on its own documentation - the explanation of
    WHY the software disable must not appear necessarily names the things it
    forbids. A guard that cries wolf at prose gets silenced, and then it is
    not a guard.
    """
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return _LINE.sub(blank, _BLOCK.sub(blank, text))


def scan(root, label, failures):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", ".pio", "test")]
        for fn in filenames:
            if not fn.endswith(SCAN_EXT):
                continue
            path = os.path.join(dirpath, fn)
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            code = strip_comments(text)
            for pattern, why in FORBIDDEN:
                for m in re.finditer(pattern, code):
                    line = code[: m.start()].count("\n") + 1
                    failures.append((path, line, why, m.group(0).strip()))


failures = []
proj = env.subst("$PROJECT_DIR")  # noqa: F821
scan(os.path.join(proj, "src"), "src", failures)
scan(os.path.join(proj, "lib"), "lib", failures)

# The BSP's startup file is the other place this can arrive from. MDK 8.45.0+
# performs the software write in system_nrf52840.c unless ENABLE_APPROTECT is
# defined; the version we pin doesn't contain the word at all.
bsp_note = "not found (skipped)"
pkg = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")  # noqa: F821
if pkg:
    sysfile = os.path.join(pkg, "cores", "nRF5", "nordic", "nrfx", "mdk",
                           "system_nrf52840.c")
    if os.path.isfile(sysfile):
        text = strip_comments(open(sysfile, encoding="utf-8",
                                   errors="replace").read())
        if "APPROTECT" in text:
            failures.append((sysfile, 0,
                             "BSP startup code now touches APPROTECT - the MDK "
                             "was bumped and may unlock the port every boot",
                             "APPROTECT"))
            bsp_note = "CONTAINS APPROTECT"
        else:
            bsp_note = "clean (no APPROTECT)"

if failures:
    sys.stderr.write(
        "\n*** thicket check_approtect.py: BUILD REFUSED\n"
        "***\n"
        "*** This firmware's debug-port protection depends on NOTHING here\n"
        "*** performing the APPROTECT software disable. Something now does:\n***\n")
    for path, line, why, tok in failures:
        rel = os.path.relpath(path, proj) if path.startswith(proj) else path
        where = f"{rel}:{line}" if line else rel
        sys.stderr.write(f"***   {where}\n***     {why}  ({tok!r})\n")
    sys.stderr.write(
        "***\n"
        "*** If this is deliberate, understand what it costs first: on Fxx+\n"
        "*** silicon it opens SWD on every boot, and anything held in internal\n"
        "*** flash - a key-derivation pepper, say - stops being protected.\n"
        "*** Do not silence this to make a debug build work - use a separate\n"
        "*** env and decide knowingly.\n\n")
    env.Exit(1)  # noqa: F821

print(f"check_approtect: no software APPROTECT disable in src/ or lib/; "
      f"BSP system_nrf52840.c {bsp_note}")
