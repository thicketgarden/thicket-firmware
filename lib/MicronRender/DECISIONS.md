# Page renderer: decisions

Why each layout value is what it is. Numbers here are the ones in
`PageMetrics`; if they disagree, the code is right and this file is stale.

## The panel decides everything

400x240, one ink, no grey, no antialiasing. Cozette advances **6 px** for every
glyph, so the usable width is 396 px after a 2 px margin each side: **66 cells**.
Line height is the font's own 13 px, giving **18 rows**.

Because the advance is fixed, a run's width is its codepoint count times six.
No measuring pass is needed and none exists.

## No layout tree, no second buffer

The page is re-flowed from its source on every frame and only rows intersecting
the scroll window are drawn. A scroll step costs a re-parse and saves the whole
page model. The framebuffer is already the largest allocation in the device;
a second page-sized structure was not going to be affordable.

The only per-page state is a **48-entry link table** so a later input layer can
hit-test without a second pass. Overflow is reported rather than silently
dropped.

## Colour to one ink

The parser reports colour as the page asked for it and resolves nothing. Here:

- **Foreground is always black.** A requested colour cannot be reproduced and
  must not be allowed to cost legibility.
- **A background inverts the row** only when its Rec. 601 luma is below half,
  which is dark enough to read white text on. Anything else is ignored.
- The rule that overrides all of it: **never black on black, never white on
  white.** Ignoring a colour is always safe; honouring one is not.

## Headings

Inverted across the full content width, then 4 px of air. Cozette is
single-weight, so inversion is the only distinction available that does not
change the type. Depth indents by 12 px, two cells, matching Micron's own rule.

## Links

Underlined, one pixel above the cell floor. Colour is not available, so the
underline carries it alone. Pixel extents are recorded per link. A link that
wraps is boxed on its last row only, which is enough to press.

## Dividers

A 1 px rule, not a row of glyphs. The parser reports the fill character the
page asked for; on this panel a rule reads better than any of them.

## Alignment

Centre and right are honoured **only where the span fits one row**. Doing it
properly needs the row's width before the row is drawn, and this renderer holds
no row. Where a span wraps, alignment is dropped rather than guessed, because a
half-centred paragraph reads worse than a left-aligned one.

## Blank lines

The parser reports no row for a blank source line, correctly: the reference
renders nothing there either. But paragraphs that run together are much worse to
read, so the driver calls `blankLine()` and the renderer opens one line of air.
Consecutive blanks collapse to one.

## Tables

Columns are **evenly divided** across the content width, not measured, because
measuring needs the whole table held and holding it is what this renderer
refuses to do. Header row inverted. A rule under the table. No box drawing: the
font has blocks but no box-drawing lines, and whitespace columns read better at
66 cells than a terminal's 100-column grid would.

⚠ **Cell content wider than its column is currently clipped.** Wrapping inside a
cell needs a variable row height, which needs the table held. Open.

## Images and partials

Phase 1. An image is `[img]` plus its alt text, no decode. A partial is `[+]`
plus a link to what would be fetched, no fetch.

## Font coverage, and the gap

`FONT_EXTRA` carries 43 non-ASCII glyphs: blocks, shades, geometric shapes,
check and cross. It has **no box-drawing lines and no accented Latin**. A
missing glyph draws blank while still advancing, so text stays aligned and
characters silently vanish. German pages in the corpus and any page drawing a
box will show holes. Not fixed here; it is a font question, not a layout one.
