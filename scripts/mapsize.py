#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Where the image went: linker-map attribution by origin and by object file.
#
# Answers "what do I delete to get 40 KB back" without guessing. The spike that
# preceded this firmware found its single largest lever (-Ofast -> -Os, worth
# 44.7% of the image) by reading this attribution, not by reasoning about the
# code.
#
# Usage:
#   python3 scripts/mapsize.py .pio/build/<env>/firmware.map [--objects N]

import os
import re
import sys

# Sections that occupy flash. .bss and .heap are RAM and are reported apart.
FLASH_SECTIONS = (".text", ".rodata", ".ARM.extab", ".ARM.exidx", ".data")
RAM_SECTIONS = (".bss", ".data")

# Longest prefix wins, so order matters only for readability.
ORIGINS = (
    ("microReticulum",   ("microReticulum",)),
    ("microLXMF",        ("microLXMF",)),
    ("microStore",       ("microStore",)),
    ("LoRaInterface",    ("LoRaInterface",)),
    ("RadioLib",         ("RadioLib",)),
    ("Crypto (rweather)", ("Crypto",)),
    ("Adafruit_SPIFlash", ("Adafruit SPIFlash", "Adafruit_SPIFlash")),
    ("SdFat",            ("SdFat",)),
    ("TinyUSB",          ("Adafruit TinyUSB", "TinyUSB")),
    ("ArduinoJson",      ("ArduinoJson",)),
    ("MsgPack",          ("MsgPack",)),
    ("Arduino core/BSP", ("FrameworkArduino", "libFrameworkArduino")),
    ("thicket src/",       ("build/wiscore", "/src/main",)),
)

LIBGCC = ("libgcc", "crtbegin", "crtend", "crti", "crtn")
LIBSTDCXX = ("libstdc++", "libsupc++")
LIBC = ("libc.a", "libc_nano", "libm.a", "libnosys", "lib_a-")


def classify(path):
    p = path.replace("\\", "/")
    for name, needles in ORIGINS:
        for n in needles:
            if n in p:
                return name
    base = os.path.basename(p)
    for n in LIBSTDCXX:
        if n in p:
            return "C++ runtime (libstdc++/libsupc++)"
    for n in LIBGCC:
        if n in p or base.startswith(n):
            return "C++ runtime (libgcc)"
    for n in LIBC:
        if n in p:
            return "libc / newlib"
    if p.endswith("/src/main.cpp.o") or "/src/" in p:
        return "thicket src/"
    return "other: " + base


def parse(path):
    """(section, object_path) -> bytes, from the `Linker script and memory map`.

    GNU ld emits input-section lines in two shapes, and both have to be handled
    or the attribution silently misses a quarter of the image:

        .text.foo      0x0000000000026100       0x2c path/to/obj.o
        .text.averyverylongsymbolname
                       0x0000000000026130       0x18 path/to/obj.o

    The second shape wraps when the section name is long enough, which, on a
    C++ codebase compiled with -ffunction-sections, is most of them.
    """
    # section name, address, size, object, all on one line
    one_line = re.compile(
        r"^\s(\.\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*?)\s*$")
    # section name alone (wrapped form)
    name_only = re.compile(r"^\s(\.\S+)\s*$")
    # address, size, object, the continuation of the wrapped form
    continuation = re.compile(
        r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*?)\s*$")

    sizes = {}
    pending = None
    # Everything before "Linker script and memory map" is the
    # "Discarded input sections" block, sections --gc-sections threw away.
    # Counting them inflates the total by more than the image itself.
    live = False

    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not live:
                if line.startswith("Linker script and memory map"):
                    live = True
                continue
            if not line or line.startswith("LOAD ") or "*fill*" in line:
                pending = None
                continue

            m = one_line.match(line)
            if m:
                pending = None
                section, _addr, size_hex, obj = m.groups()
                size = int(size_hex, 16)
                if size and (obj.endswith(".o") or obj.endswith(")")):
                    key = (section, obj)
                    sizes[key] = sizes.get(key, 0) + size
                continue

            m = name_only.match(line)
            if m:
                pending = m.group(1)
                continue

            m = continuation.match(line)
            if m and pending:
                _addr, size_hex, obj = m.groups()
                size = int(size_hex, 16)
                if size and (obj.endswith(".o") or obj.endswith(")")):
                    key = (pending, obj)
                    sizes[key] = sizes.get(key, 0) + size
                pending = None
                continue

            pending = None

    return sizes


def report(sizes, sections, title, top_objects):
    by_origin = {}
    by_object = {}
    total = 0
    for (section, obj), size in sizes.items():
        # Sections arrive as -ffunction-sections subsections
        # (".text._ZN3RNS9Transport...", ".ARM.extab.text._ZN..."), so match on
        # prefix. Longest prefix first so .ARM.extab isn't eaten by .ARM.exidx.
        if not any(section == s or section.startswith(s + ".")
                   for s in sections):
            continue
        total += size
        by_origin[classify(obj)] = by_origin.get(classify(obj), 0) + size
        by_object[obj] = by_object.get(obj, 0) + size

    print("")
    print("== %s ==" % title)
    print("attributed total: %d B" % total)
    print("")
    print("%-38s %10s %8s" % ("origin", "bytes", "share"))
    print("-" * 58)
    for name, size in sorted(by_origin.items(), key=lambda kv: -kv[1]):
        print("%-38s %10d %7.1f%%" % (name, size, 100.0 * size / total if total else 0))

    if top_objects:
        print("")
        print("top %d objects" % top_objects)
        print("-" * 58)
        for obj, size in sorted(by_object.items(), key=lambda kv: -kv[1])[:top_objects]:
            short = obj
            if "/" in short:
                short = "/".join(short.split("/")[-2:])
            print("%-48s %8d" % (short, size))


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: mapsize.py <firmware.map> [--objects N]\n")
        return 2

    path = sys.argv[1]
    top = 15
    if "--objects" in sys.argv:
        top = int(sys.argv[sys.argv.index("--objects") + 1])

    sizes = parse(path)
    report(sizes, FLASH_SECTIONS, "FLASH", top)
    report(sizes, RAM_SECTIONS, "STATIC RAM (.data + .bss)", top)
    print("")
    print("Reminder: the RNS TLSF pool (RNS_HEAP_POOL_BUFFER_SIZE) is malloc'd")
    print("at runtime by Utilities/Memory.cpp pool_init(), so it appears in")
    print("NEITHER table above. Add it by hand when budgeting RAM.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
