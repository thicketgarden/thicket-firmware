#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Convert a BDF bitmap font to a C header for the display driver.
# ASCII 32..126 only; one byte per row, bit 7 leftmost.
#
#   python3 scripts/bdf2header.py cozette.bdf lib/SharpLcd/src/CozetteFont.h

import re
import sys

FIRST, LAST = 32, 126

# Beyond ASCII, WHOLE RANGES rather than a hand-picked list.
#
# A curated list saves flash and costs correctness: a page using one character
# nobody thought of renders a hole, and the hole is silent because a missing
# glyph still advances. Real corpus pages hit exactly that with German text.
# Ranges are cheap enough that picking is not worth the risk.
#
# Everything here is verified present in Cozette; the generator fails loudly if
# a range it was asked for is not.
EXTRA_RANGES = [
    (0x00A0, 0x00FF, "Latin-1 Supplement"),      # accented Latin, punctuation
    (0x0100, 0x017F, "Latin Extended-A"),        # the rest of European Latin
    (0x2500, 0x257F, "Box Drawing"),             # literal page content, and
                                                 # box-drawn tables as an option
    (0x2580, 0x259F, "Block Elements"),
    (0x25A0, 0x25FF, "Geometric Shapes"),
]

# Odds and ends outside those blocks that the UI already draws.
EXTRA_SINGLES = [
    0x2190, 0x2191, 0x2192, 0x2193,                        # arrows
    0x2022, 0x2026,                                        # bullet, ellipsis
    0x2661, 0x2665, 0x2713, 0x2717,                        # hearts, tick, cross
]

EXTRA = sorted(set(
    [c for lo, hi, _ in EXTRA_RANGES for c in range(lo, hi + 1)] + EXTRA_SINGLES))



def parse(path):
    ascent = descent = None
    glyphs = {}
    enc = bbx = dwidth = None
    bits = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("FONT_ASCENT"):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT"):
            descent = int(line.split()[1])
        elif line.startswith("ENCODING"):
            enc = int(line.split()[1])
        elif line.startswith("DWIDTH") and enc is not None:
            dwidth = int(line.split()[1])
        elif line.startswith("BBX"):
            bbx = [int(v) for v in line.split()[1:]]
        elif line == "BITMAP":
            bits = []
        elif line == "ENDCHAR":
            keep = (enc is not None and
                    (FIRST <= enc <= LAST or enc in EXTRA))
            if bits is not None and keep:
                glyphs[enc] = (bbx, bits, dwidth)
            enc = bbx = bits = dwidth = None
        elif bits is not None and re.fullmatch(r"[0-9A-Fa-f]+", line):
            bits.append(line)
    # Two different widths, and conflating them clips the box-drawing set.
    # DWIDTH is the ADVANCE (6). BBX width is how far the ink reaches, and
    # box/block glyphs are 7 wide on purpose so neighbouring cells touch and
    # rules join up. Store the ink width; lay out on the advance.
    #
    # The advance is the MODE, not the maximum: Cozette carries a handful of
    # double-width glyphs, and one of them would otherwise redefine the cell for
    # the whole font.
    from collections import Counter
    advance = Counter(g[2] for g in glyphs.values()).most_common(1)[0][0]

    # A glyph that is not one cell wide cannot sit in a monospace grid. Drop it
    # and say so; it falls back to blank-but-advancing like any other unknown,
    # which keeps layout intact.
    oversize = sorted(c for c, g in glyphs.items()
                      if g[2] != advance or g[0][0] + max(0, g[0][2]) > 8)
    for c in oversize:
        del glyphs[c]

    ink_w = max(g[0][0] + max(0, g[0][2]) for g in glyphs.values())
    return ascent, descent, advance, ink_w, glyphs, oversize


def render(bbx, rows, dwidth, cell_w, cell_h, ascent):
    """Place a glyph's bitmap into a fixed cell, top-left origin."""
    gw, gh, xoff, yoff = bbx
    out = [0] * cell_h
    for i, hexrow in enumerate(rows):
        # BDF pads each row to whole bytes, MSB leftmost.
        val = int(hexrow, 16)
        nbits = len(hexrow) * 4
        row = [(val >> (nbits - 1 - b)) & 1 for b in range(min(nbits, gw))]
        y = ascent - yoff - gh + i          # baseline-relative to top-left
        if not (0 <= y < cell_h):
            continue
        acc = 0
        for x, on in enumerate(row):
            px = x + xoff
            if on and 0 <= px < cell_w:
                acc |= 0x80 >> px
        out[y] |= acc
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: bdf2header.py IN.bdf OUT.h")
    src, dst = sys.argv[1], sys.argv[2]
    ascent, descent, advance, cell_w, glyphs, oversize = parse(src)
    cell_h = ascent + descent

    missing = [c for c in list(range(FIRST, LAST + 1)) + EXTRA
               if c not in glyphs and c not in oversize]
    if missing:
        sys.exit(f"{src}: missing glyphs for {missing}")
    if oversize:
        print(f"[bdf2header] skipped {len(oversize)} glyph(s) wider than one cell: "
              + " ".join(f"U+{c:04X}" for c in oversize), file=sys.stderr)
    if cell_w > 8:
        sys.exit(f"{src}: ink width {cell_w} does not fit one byte per row")

    lines = [
        "// Generated by scripts/bdf2header.py -- do not edit.",
        f"// Source: {src.split('/')[-1]}, Cozette by Ines (MIT).",
        f"// advance {advance}, ink {cell_w}x{cell_h}, ascent {ascent}, bit 7 leftmost.",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "namespace thicket {",
        "",
        f"static const uint8_t FONT_W = {advance};        // advance",
        f"static const uint8_t FONT_INK_W = {cell_w};",
        f"static const uint8_t FONT_H = {cell_h};",
        f"static const uint8_t FONT_FIRST = {FIRST};",
        f"static const uint8_t FONT_LAST = {LAST};",
        "",
        f"static const uint8_t FONT[{LAST - FIRST + 1}][{cell_h}] = {{",
    ]
    for c in range(FIRST, LAST + 1):
        rows = render(*glyphs[c], cell_w, cell_h, ascent)
        body = ",".join(f"0x{v:02X}" for v in rows)
        ch = chr(c).replace("\\", "backslash")
        lines.append(f"\t{{{body}}},  // {c} {ch!r}" if c != 39 else
                     f"\t{{{body}}},  // {c} apostrophe")
    lines += ["};", ""]

    # Extended glyphs: sorted, looked up by codepoint. Anything the font does
    # not carry at one cell wide is simply absent, and falls back to
    # blank-but-advancing at draw time.
    extra = sorted(c for c in EXTRA if c in glyphs)
    lines += [
        "struct FontExtra { uint16_t cp; uint8_t rows[%d]; };" % cell_h,
        "",
        f"static const uint16_t FONT_EXTRA_COUNT = {len(extra)};",
        f"static const FontExtra FONT_EXTRA[{len(extra)}] = {{",
    ]
    for c in extra:
        rows = render(*glyphs[c], cell_w, cell_h, ascent)
        body = ",".join(f"0x{v:02X}" for v in rows)
        lines.append(f"\t{{0x{c:04X}, {{{body}}}}},  // {chr(c)}")
    lines += ["};", "", "}  // namespace thicket", ""]

    open(dst, "w").write("\n".join(lines))
    print(f"[bdf2header] {cell_w}x{cell_h} cell, advance {advance}: "
          f"{LAST-FIRST+1} ASCII + {len(extra)} extended -> {dst}")
    for lo, hi, name in EXTRA_RANGES:
        have = sum(1 for c in extra if lo <= c <= hi)
        print(f"[bdf2header]   {name:22} {have:4} of {hi - lo + 1}")


if __name__ == "__main__":
    main()
