# Page renderer: decisions

Why each layout value is what it is. Numbers here are the ones in
`PageMetrics`; if they disagree, the code is right and this file is stale.

## The panel decides everything

400x240, one ink, no grey, no antialiasing. Cozette advances **6 px** for every
glyph, so the usable width is 396 px after a 2 px margin each side: **66 cells**.
Line height is the font's own 13 px **plus 2 px of leading**, giving **16
rows**. The two rows that costs buy a dense page that reads markedly better;
both were rendered and compared.

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

## Headings and bold: a Tamzen ladder over a Cozette body

Cozette is the body face and the fallback face. Tamzen supplies the one thing
Cozette lacks, weight, as three bold faces:

| level | face | cell |
|---|---|---|
| body | Cozette | 6x13 |
| inline `! bold | Tamzen bold | 6x12 |
| H3 | Tamzen bold | 7x13 |
| H2 | Tamzen bold | 8x16 |
| H1 | Cozette hi-DPI | 12x26 |

**Bold switches to Tamzen; underline stays Cozette with a drawn rule.** Stacked,
bold+underline is the Tamzen face with the rule on top, because the face comes
from bold and the rule from underline, independently. Underline alone is not
heavier, only ruled.

**Inline bold is drawn in Tamzen 6x12 bold and falls back to Cozette per glyph.**
Both advance 6, so the horizontal grid holds within a line, and both have
ascent 10, so a bold word sits on the same baseline as the Cozette around it.
The shear question never arises: it only applied to box art on a mixed body
line, and box art is never bolded. A codepoint Tamzen lacks (a box or block
glyph) simply draws in Cozette.

**Headings are whole-line-or-nothing.** A heading is drawn entirely in one
Tamzen face; if any codepoint on the line is missing from that face the whole
row drops to Cozette body. So two sizes never share a heading line, and a
fallback cannot misalign one. In corpus practice this never fires: no heading
line contains a non-Latin glyph.

**Rules are now a fallback cue, not the treatment.** Size and weight carry the
ladder. A rule under a heading is drawn only when the line dropped to body size,
which is the one case where nothing else distinguishes it.

Inversion is gone from headings entirely, reserved for a dark background the
page asked for. Each treatment means one thing: size and weight for hierarchy,
inversion for a colour the page chose.

**Cost:** three Tamzen bold faces, ASCII and Latin-1, 8.2 KB, on top of
Cozette's 9 KB and the hi-DPI face's 10.3 KB.

## Links

Micron has one link grammar, `` `[label`target] ``, and the reference adds no
style of its own to it. Checked against NomadNet 1.4.0: a plain link renders
`fg #dddddd`, the same as body text, with no underline and no colour flag. Its
`LinkSpec` copies the surrounding style verbatim. A `` `_ `` word, by contrast,
carries an explicit underline flag. So a link is only distinct when the page
author made it so, by convention `` `F79d`_`[..]`_`f `` (teal plus underline);
NomadNet's own `Channels.py` carries that `79d` as its default link colour.

We render the same way: a link inherits the surrounding style and adds nothing.
An author-styled link keeps its underline, because the author wrote `` `_ ``, and
its colour resolves to ink like any other text. A plain link reads as body text,
exactly as in the reference. The label's pixel extents are still recorded per
link for a later input layer to hit-test.

The reference distinguishes links during navigation with reverse-video on the
focused link, not with static styling. That is the pending input-layer behaviour
(TODO in `emit_run`); until there is input there is no focused link to draw.

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

Laid out here, because layout is what a renderer is for and the parser
deliberately refuses to do it.

**Rows are collected into a fixed buffer, then laid out at the closing toggle.**
Column widths need the widest cell in each column, which needs the whole table.
A table is not a page though: 16 rows, 6 columns and 1 KB of text hold any
sensible one, and anything past that is **reported** through
`table_overflowed()` rather than silently truncated.

**No column may take more than half the width** before fitting. One long cell
otherwise absorbs everything proportional scaling has to give: a 60-cell price
column squeezed a `Qty` header down to two cells and broke it across two lines
for nothing. After capping, columns take their natural width if the table fits
and are scaled proportionally if not, with a two-cell floor.

**A cell wider than its column WRAPS, and the row gets taller.** Clipping loses
the reader something a taller row would have shown. Wrapping is word-aware, and
backs up to a space only when that still fills two thirds of the line, because
in a narrow column a word break can waste more than it saves. The line count is
computed by walking with the same rule the drawing uses; a plain divide
under-counts when a word break pushes a cell onto another line, and a row too
short overlaps the one beneath it.

**Columns are separated by a drawn 1px rule**, one per gutter, stacked per line
so it scrolls and clips for free. A hairline the pixel device draws directly,
where a terminal can only stack box glyphs. Whitespace alignment still does the
column widths; the rule just makes the boundary explicit.

**The header is ruled, not inverted**, matching the heading decision: inversion
means a dark background the page asked for and nothing else. A second rule
closes the table.

No box drawing. The font now carries it, and whitespace columns still read
better at 66 cells than a terminal's 100-column grid.

## Images and partials

Phase 1. An image is `[img]` plus its alt text, no decode. A partial is `[+]`
plus a link to what would be fetched, no fetch.

## Font coverage, and the gap

`FONT_EXTRA` carries 43 non-ASCII glyphs: blocks, shades, geometric shapes,
check and cross. It has **no box-drawing lines and no accented Latin**. A
missing glyph draws blank while still advancing, so text stays aligned and
characters silently vanish. German pages in the corpus and any page drawing a
box will show holes. Not fixed here; it is a font question, not a layout one.

---

# Design direction

Where rendering goes next. The framing: this is a pixel-addressable device with
a bespoke renderer, not a character grid. Micron can be presented *better* here
than in a terminal for the content that matters, rather than merely degraded to
one ink. Each item below is marked **cheap win** or **real project**.

## 1. Dither, not threshold: DONE, cheap win

Shipped. A background is painted with a 4x4 ordered dither at its luma, so
brightness survives as texture below the size of a character cell, which is the
smallest thing a terminal can colour.

Measured on a sixteen-step ramp: thresholding gives two solid bars that stop
dead at the halfway point; dithering gives the ramp. Legibility still wins where
they collide, via a knockout under each glyph.

## 2. Drawn rules instead of glyph rules: MIXED, and the reference wins

Dividers now draw the fill character the page asked for, because the reference
does and three fills were collapsing into one rule. On this panel that is also
the better picture: Cozette's box glyphs are 7 px of ink against a 6 px advance,
so neighbouring cells touch and a row of `U+2500` IS a continuous hairline. A
drawn rule and a glyph rule are the same pixels.

**Where a drawn rule genuinely wins is where no glyph exists**: the table header
underline and the closing rule are drawn, at 1 px, positioned to the row rather
than snapped to a cell. That is already the case.

⚠ Remaining opportunity, **real project**: column separators and cell borders in
tables are currently whitespace. Hairlines between columns would be a picture no
terminal can draw. Not built; whitespace reads well enough at 66 cells that it
has not earned the complexity yet.

## 3. Per-service rendering: the detection question, INVESTIGATED ONLY

The opportunity is real and no terminal client can do it. The blocker is
knowing what a page **is**. What exists today:

- **`#!` page directives are the only metadata channel, and they are
  extensible.** NomadNet defines exactly three, parsed positionally at the top
  of a page: `#!c=` cache seconds, `#!bg=`, `#!fg=`. Real pages in the corpus
  use all three. **There is no type directive**, and adding `#!type=board`
  unilaterally is a protocol proposal rather than a local decision. It is also
  the obvious right place, and cheap for a node operator to add.
- **Naming is a weak convention.** `index` appears 12 times across real nodes;
  everything else is a long tail of one or two. There is no `board.mu` habit to
  key off.
- **Structure is suggestive but not decisive.** A board is a repeated
  heading-plus-body run; a form-heavy page has many `` `< `` fields; a listing is
  a table. Across the corpus these separate cleanly enough to *guess* and not
  cleanly enough to *rely on*: one page has 36 headings, 22 fields and 4 tables
  and is a documentation template, not an application.
- **The announce carries no page-type field.** Interface discovery gained an
  operator contact recently, so the announce is where node-level metadata goes,
  but it describes a node, not a page.

**Recommendation:** treat detection as a proposal, not a heuristic. A
`#!type=` directive costs a node operator one line, matches an existing
mechanism, and degrades to nothing on clients that ignore it. Guessing from
structure would present a documentation page as a message board, which is worse
than presenting everything generically.

## 4. Tamzen as a second face: metrics MEASURED, viable with caveats

The question that decides it: do the cells line up when a Cozette glyph falls
back inside Tamzen text? Measured against Tamzen 1.11.5:

| face | advance | cell | ascent | box drawing | blocks |
|---|---|---|---|---|---|
| Cozette | 6 | 13 | 10 | 128 | 32 |
| Tamzen 6x12 r/b | **6** | 12 | 10 | **0** | **0** |
| Tamzen 7x13 | 7 | 13 | 11 | 0 | 0 |
| Tamzen 8x16 | 8 | 16 | 12 | 0 | 0 |

**Only 6x12 is a candidate.** Its advance matches Cozette's exactly, so a
fallback glyph keeps the horizontal grid. 7x13 and 8x16 shear art horizontally
and are out for mixed content.

**The 1 px height difference is safe**, which the numbers alone did not settle.
A 12 px row pitch OVERLAPS a 13 px glyph rather than leaving a gap, and a frame
drawn at both pitches still connects. A gap would have broken it; an overlap
does not.

⚠ **Tamzen carries no box drawing and no block elements at all**, so on a page
built out of art, essentially everything falls back. That is coherent rather
than fatal: Tamzen would supply Latin text, where the gain is, and Cozette would
supply the art, where it already wins.

**What it buys, part one: real inline bold** at a matching advance, which
Cozette cannot do at any size.

**Part two: a heading size ladder, which the 2x face does not provide.** The
hi-DPI face covers H1 and nothing else. H2 and H3 render at body size today and
are separated by rules alone, so there are two type sizes on the device, not
four.

The advance-mismatch objection does **not** apply here, and conflating the two
cases is what made this look impossible. Mismatched advances shear art on a
BODY line, where prose and box drawing share a row. **A heading line is pure
text**: across the whole corpus, zero heading lines contain a box or block
glyph. Tamzen 7x13 and 8x16 are therefore usable for H2 and H3 even though they
are unusable for body text.

That gives a real ladder:

| level | face | cell |
|---|---|---|
| H1 | Cozette hi-DPI | 12x26 |
| H2 | Tamzen | 8x16 |
| H3 | Tamzen | 7x13 |
| body | Cozette | 6x13 |

Each heading keeps the **whole-line-or-nothing** fallback the 2x face already
uses: if any codepoint on the line is missing from the heading face, the entire
line drops to body size. Two sizes never mix within one line, so a fallback
cannot misalign a heading.

**Cost:** roughly 5.3 KB for 6x12 regular and bold, and roughly 6.3 KB more for
7x13 and 8x16, against Cozette's 9 KB and the hi-DPI face's 10.3 KB.

**Verdict: a real project, and now worth more than bold alone.** Bold and a
four-step size ladder together are the case; either on its own is thinner. Not
started, and the metrics no longer block it.


## Parity is only as strong as the corpus AND the comparison

Three bugs in fields and dividers survived a green parity run, and the reason is
worth keeping because the obvious explanation is wrong. It was not that the
corpus lacked cases: it already held four pages with a pre-checked box, five
with a masked field and seven with a multi-byte divider fill.

**Those constructs were excluded from the diff on both sides.** The reference
returns fields and dividers as widgets rather than through the text path, so
they were skipped, and parity passed over them by construction. Adding pages
would not have caught anything; only comparing the constructs did, and doing so
surfaced the differences immediately.

**The renderer gallery is a second oracle.** All three bugs were found by
looking at a rendered page and asking why it was wrong, not by the parity suite.
A parser diff and a picture fail differently, and the picture found what the
diff was configured not to look at.

**So when the renderer finds a parser bug, the fix has three parts:** correct
the parser in micron-cpp, make the parity harness compare the construct, and
only then call it closed. Fixing the parser alone leaves parity green over the
same hole.
