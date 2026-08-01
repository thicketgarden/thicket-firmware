#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# What actually lands in the application region, measured from the Intel HEX.
#
# `pio run` prints a Flash number that sums .text + .ARM.exidx + .data and
# OMITS .ARM.extab — tens of kilobytes of exception unwind tables on this
# stack, because microReticulum throws. It also measures against
# boards/rak4630.json's `maximum_size`, which is right for the SoftDevice build
# and wrong for the no-BLE one.
#
# The HEX is the artifact that gets flashed, so the HEX is what we quote.
#
# Usage:
#   python3 scripts/hexsize.py .pio/build/<env>/firmware.hex [region_bytes]

import sys


def hex_extent(path):
    """(lowest_address, highest_address_exclusive, bytes_of_data) of an Intel HEX."""
    base = 0
    lo = None
    hi = 0
    total = 0

    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":"):
                continue
            count = int(line[1:3], 16)
            offset = int(line[3:7], 16)
            rectype = int(line[7:9], 16)
            data = line[9:9 + count * 2]

            if rectype == 0x00:          # data
                addr = base + offset
                lo = addr if lo is None else min(lo, addr)
                hi = max(hi, addr + count)
                total += count
            elif rectype == 0x04:        # extended linear address
                base = int(data, 16) << 16
            elif rectype == 0x02:        # extended segment address
                base = int(data, 16) << 4

    return (lo or 0), hi, total


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: hexsize.py <firmware.hex> [region_bytes]\n")
        return 2

    path = sys.argv[1]
    # 815,104 B with SoftDevice S140 (app starts at 0x26000);
    # 966,656 B without it. Pass the right one for the env being measured.
    region = int(sys.argv[2]) if len(sys.argv) > 2 else 815104

    lo, hi, payload = hex_extent(path)
    span = hi - lo

    print("file          : %s" % path)
    print("start address : 0x%06X" % lo)
    print("end address   : 0x%06X" % hi)
    print("image span    : %d B   <- this is the number to quote" % span)
    print("payload bytes : %d B   (span minus inter-section gaps)" % payload)
    print("app region    : %d B" % region)
    print("used          : %.2f%%" % (100.0 * span / region))
    print("free          : %d B" % (region - span))
    return 0


if __name__ == "__main__":
    sys.exit(main())
