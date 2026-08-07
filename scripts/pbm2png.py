#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Convert the virtual panel's P1 PBM to PNG. Uses only zlib/struct so it needs
# no third-party imaging library.
#
#   python3 scripts/pbm2png.py screen.pbm screen.png

import struct
import sys
import zlib


def read_pbm(path):
    tok = []
    with open(path, "rb") as fh:
        for raw in fh:
            line = raw.split(b"#", 1)[0].strip()
            if line:
                tok.append(line)
    if tok[0] != b"P1":
        sys.exit(f"{path}: not a P1 PBM")
    w, h = (int(v) for v in tok[1].split())
    bits = b"".join(tok[2:])
    bits = bytes(c for c in bits if c in (48, 49))
    if len(bits) != w * h:
        sys.exit(f"{path}: expected {w*h} pixels, found {len(bits)}")
    return w, h, bits


def write_png(path, w, h, bits):
    # 8-bit greyscale: PBM 1 = black.
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter: none
        row = bits[y * w:(y + 1) * w]
        rows.extend(0 if c == 49 else 255 for c in row)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)
    return len(png)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: pbm2png.py IN.pbm OUT.png")
    w, h, bits = read_pbm(sys.argv[1])
    n = write_png(sys.argv[2], w, h, bits)
    print(f"[pbm2png] {w}x{h} -> {sys.argv[2]} ({n:,} bytes)")
