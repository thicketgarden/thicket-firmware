#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Worst-case stack frames, from GCC's -fstack-usage.
#
# Why this exists alongside the on-board measurement: uxTaskGetStackHighWaterMark
# reports how deep the stack HAS been, which only covers paths that actually
# ran. This reports how deep single frames CAN be, including code no test has
# reached yet. A stack overflow on this part presents as a hard fault at a
# random address with no diagnostic, so the paths that haven't run are exactly
# the ones worth bounding.
#
# It reports frames, not chains. GCC emits per-function sizes; turning those
# into a true worst-case depth needs the call graph, and indirect calls and
# recursion make that an over-approximation anyway. A single 1 KB frame in a
# 4 KB task stack is actionable on its own.
#
# The `dynamic` qualifier is the one to read first: it means alloca or a
# variable-length array, so the frame has no static bound at all.
#
# Usage:
#   pio run -e <env> with -fstack-usage in build flags, then
#   python3 scripts/stackusage.py [env] [--top N]

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapsize import classify  # noqa: E402


def collect(build_dir):
    rows = []
    for dirpath, _dirnames, filenames in os.walk(build_dir):
        for fn in filenames:
            if not fn.endswith(".su"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, errors="replace") as fh:
                    for line in fh:
                        parts = line.rstrip("\n").split("\t")
                        if len(parts) < 3:
                            continue
                        where, size, qual = parts[0], parts[1], parts[2]
                        try:
                            size = int(size)
                        except ValueError:
                            continue
                        # "path/file.cpp:LINE:COL:signature" -- the signature
                        # itself contains colons (C++ scope), so split from the
                        # LEFT exactly three times and keep the rest intact.
                        bits = where.split(":", 3)
                        if len(bits) == 4:
                            src, func = bits[0], bits[3]
                        else:
                            src, func = where, where
                        rows.append((size, qual, func.strip(), src, path))
            except OSError:
                continue
    return rows


def image_symbols(elf):
    """Demangled names of functions actually present in the final image.

    Without this the report is misleading: .su files are emitted per compiled
    function, including every one that --gc-sections later discarded. The
    deepest frame in this build (a 3,296 B compression encoder) is exactly such
    a case -- compiled, never linked, and not a risk. Flagging what survived
    separates a real hazard from a dead one.
    """
    import subprocess
    nm = None
    root = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi")
    for dirpath, _d, filenames in os.walk(root):
        if "arm-none-eabi-nm" in filenames:
            nm = os.path.join(dirpath, "arm-none-eabi-nm")
            break
    if not nm or not os.path.exists(elf):
        return None
    out = subprocess.run([nm, "-C", "--defined-only", elf],
                         capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3:
            # strip the argument list; scope-qualified name is enough
            nm_name = parts[2].split("(")[0].strip()
            if nm_name:
                names.add(nm_name)
    return names


def qualified(sig):
    """Best-effort scope-qualified name out of a .su signature."""
    head = sig.split("(")[0].strip()
    return head.split(" ")[-1] if head else sig


def main():
    args = sys.argv[1:]
    top = 20
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]
    env = args[0] if args else "wiscore_rak4631-noflash"

    build = os.path.join(".pio", "build", env)
    rows = collect(build)
    if not rows:
        sys.exit(f"no .su files under {build}; rebuild with -fstack-usage in "
                 f"the build flags (PLATFORMIO_BUILD_FLAGS=-fstack-usage)")

    syms = image_symbols(os.path.join(build, "firmware.elf"))

    def live(sig):
        if syms is None:
            return None
        return qualified(sig) in syms

    print(f"=== stack usage: {env} ===\n")
    print(f"  {len(rows)} functions with a reported frame")
    if syms is not None:
        n_live = sum(1 for r in rows if live(r[2]))
        print(f"  {n_live} of them survive into the image; the rest were "
              f"discarded by --gc-sections")
    print()

    dynamic = [r for r in rows if "dynamic" in r[1]]
    bounded = [r for r in rows if "bounded" in r[1]]
    print(f"  static frames        {len(rows) - len(dynamic) - len(bounded):>6}")
    print(f"  bounded frames       {len(bounded):>6}   (compiler proved a bound)")
    print(f"  DYNAMIC frames       {len(dynamic):>6}   (alloca / VLA, unbounded)\n")

    if dynamic:
        print("  ⚠ dynamic frames have no static bound. Largest reported sizes:")
        for size, _q, func, src, _p in sorted(dynamic, reverse=True)[:8]:
            print(f"      {size:>7,} B  {func[:70]}")
            print(f"                  {os.path.basename(src)}")
        print()

    print(f"=== largest {top} frames ===\n")
    for size, qual, func, src, _p in sorted(rows, reverse=True)[:top]:
        flag = " ⚠DYNAMIC" if "dynamic" in qual else ""
        lv = live(func)
        mark = "" if lv is None else ("  [in image]" if lv else "  [discarded]")
        print(f"  {size:>7,} B{flag}{mark}")
        print(f"              {func[:88]}")
        print(f"              {os.path.basename(src)}")

    by_origin = {}
    for size, _q, _f, _s, path in rows:
        by_origin.setdefault(classify(path), []).append(size)
    print("\n=== deepest single frame, by origin ===\n")
    ranked = sorted(by_origin.items(), key=lambda kv: -max(kv[1]))
    for name, sizes in ranked[:12]:
        print(f"  {name:<36} {max(sizes):>7,} B   ({len(sizes)} functions)")


if __name__ == "__main__":
    main()
