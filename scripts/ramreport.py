#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Where the RAM went. The counterpart to mapsize.py, which answers the same
# question for flash.
#
# It exists because `arm-none-eabi-size` is actively misleading on this target:
# it reports the linker's .heap section inside `bss`, so a build with 22 KB of
# real static RAM reads as 232 KB and looks like it's about to overflow a
# 237 KB part. The section table tells the truth and this script reads that.
#
# The distinction that matters on nRF52840:
#
#   .data + .bss   RAM the image actually consumes before main() runs.
#   .heap          NOT consumption. The linker sizes it to fill whatever is
#                  left, so it's the BUDGET malloc draws from, and it grows
#                  when static RAM shrinks. Reporting it as "used" hides the
#                  only number that matters, which is what remains inside it.
#
# Usage:
#   python3 scripts/ramreport.py [env] [--objects N]
#   python3 scripts/ramreport.py wiscore_rak4631 --objects 15

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapsize import parse, classify  # noqa: E402

# nRF52840 physical SRAM. Anything below RAM ORIGIN is reserved for the
# SoftDevice, whether or not a SoftDevice is actually present on the part.
SRAM_BASE = 0x20000000
SRAM_TOP = 0x20040000

RAM_SECTIONS = (".data", ".bss")


def find_tool(name):
    root = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi")
    for dirpath, _dirnames, filenames in os.walk(root):
        if name in filenames:
            return os.path.join(dirpath, name)
    return None


def sections(elf):
    """(name, addr, size) for allocated sections, from the ELF section table."""
    readelf = find_tool("arm-none-eabi-readelf")
    if not readelf:
        sys.exit("arm-none-eabi-readelf not found under ~/.platformio")
    out = subprocess.run([readelf, "-S", "-W", elf],
                         capture_output=True, text=True).stdout
    found = []
    # [ 5] .bss   NOBITS   20006cd4 076cd4 00578c 00  WA  0  0  8
    pat = re.compile(r"^\s*\[\s*\d+\]\s+(\.\S+)\s+(\S+)\s+([0-9a-f]+)\s+"
                     r"[0-9a-f]+\s+([0-9a-f]+)", re.M)
    for m in pat.finditer(out):
        name, _typ, addr, size = m.groups()
        found.append((name, int(addr, 16), int(size, 16)))
    return found


def human(n):
    return f"{n:,}"


def main():
    args = [a for a in sys.argv[1:]]
    top_objects = 12
    if "--objects" in args:
        i = args.index("--objects")
        top_objects = int(args[i + 1])
        del args[i:i + 2]
    env = args[0] if args else "wiscore_rak4631"

    build = os.path.join(".pio", "build", env)
    elf = os.path.join(build, "firmware.elf")
    mapf = os.path.join(build, "firmware.map")
    if not os.path.exists(elf):
        sys.exit(f"no ELF at {elf} -- build it first: pio run -e {env}")

    secs = {n: (a, s) for n, a, s in sections(elf)}
    data_addr, data_sz = secs.get(".data", (0, 0))
    _bss_addr, bss_sz = secs.get(".bss", (0, 0))
    heap_addr, heap_sz = secs.get(".heap", (0, 0))

    ram_origin = data_addr
    reserved = ram_origin - SRAM_BASE
    region = SRAM_TOP - ram_origin
    static = data_sz + bss_sz
    # Whatever the linker left above the heap is the main stack. On this BSP
    # that's the ISR/startup stack; FreeRTOS task stacks are allocated out of
    # .bss or the heap and are counted there instead.
    stack = SRAM_TOP - (heap_addr + heap_sz) if heap_sz else 0

    print(f"=== RAM report: {env} ===\n")
    print(f"  nRF52840 SRAM                 {human(SRAM_TOP - SRAM_BASE):>10} B")
    print(f"  reserved below RAM ORIGIN     {human(reserved):>10} B"
          f"   (SoftDevice window, 0x{SRAM_BASE:08x}-0x{ram_origin:08x})")
    print(f"  linker RAM region             {human(region):>10} B"
          f"   (0x{ram_origin:08x}-0x{SRAM_TOP:08x})\n")

    print(f"  .data  (initialised)          {human(data_sz):>10} B")
    print(f"  .bss   (zeroed)               {human(bss_sz):>10} B")
    print(f"  {'-' * 44}")
    print(f"  static RAM before main()      {human(static):>10} B"
          f"   {static / region * 100:5.1f}% of region\n")
    print(f"  .heap  (malloc budget)        {human(heap_sz):>10} B"
          f"   {heap_sz / region * 100:5.1f}% of region")
    print(f"  main stack                    {human(stack):>10} B")
    accounted = static + heap_sz + stack
    print(f"  {'-' * 44}")
    print(f"  accounted                     {human(accounted):>10} B"
          f"   (region - accounted = {human(region - accounted)} B)\n")

    if reserved:
        print(f"  NOTE: {human(reserved)} B sits below RAM ORIGIN and is "
              f"unavailable to this image.")
        print(f"        On a build with no SoftDevice on the part that is "
              f"reclaimable; see docs.\n")

    if not os.path.exists(mapf):
        print(f"(no map at {mapf}; skipping attribution)")
        return

    sizes = parse(mapf)
    by_origin = {}
    by_object = {}
    for (section, obj), size in sizes.items():
        base = section.split(".")[1] if section.count(".") >= 1 else section
        if "." + base not in RAM_SECTIONS:
            continue
        origin = classify(obj)
        by_origin[origin] = by_origin.get(origin, 0) + size
        by_object[obj] = by_object.get(obj, 0) + size

    total_attributed = sum(by_origin.values())
    print("=== static RAM (.data + .bss) by origin ===\n")
    for name, size in sorted(by_origin.items(), key=lambda kv: -kv[1]):
        if not size:
            continue
        print(f"  {name:<36} {human(size):>9} B  {size / static * 100:5.1f}%")
    print(f"  {'-' * 58}")
    print(f"  {'attributed':<36} {human(total_attributed):>9} B")
    print(f"  {'section total':<36} {human(static):>9} B")

    print(f"\n=== largest {top_objects} objects by static RAM ===\n")
    for obj, size in sorted(by_object.items(), key=lambda kv: -kv[1])[:top_objects]:
        print(f"  {human(size):>8} B  {os.path.basename(obj)}")


if __name__ == "__main__":
    main()
