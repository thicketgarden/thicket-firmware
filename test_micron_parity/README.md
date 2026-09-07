# Micron parity: our parser against NomadNet's own

`test/test_micron/` has 36 unit tests, and they assert against **hand-written
expected values**. A human read `MicronParser.py` and wrote down what they
believed it does. That proves we agree with our own reading of the grammar. It
cannot catch a misreading, because the expectation encodes the same misreading.

This harness closes that. It drives **NomadNet's actual parser** over a corpus
and diffs its output against ours, span for span.

```sh
bash test_micron_parity/run_parity.sh          # builds a throwaway venv with uv
MICRON_PY=/path/to/python run_parity.sh        # or reuse one that has nomadnet
```

## How the reference is driven

`parse_line()` is called per line and its internal `make_part()` is
intercepted, so the events are the reference's own styled spans rather than our
reading of them. No parsing logic is patched.

NomadNet reaches for the running application in four places, all rendering
concerns: a theme, an urwid screen to register palette entries against, and a
colour mode. A stub supplies exactly those. Colour mode is `COLORMODE_TRUE` so
the reference doesn't quantise colour to a terminal palette.

Our side compiles with a bare compiler and nothing else:

```sh
c++ -std=c++17 -I lib/Micron/src ours_dump.cpp lib/Micron/src/Micron.cpp
```

If that ever needs more than a C++17 compiler and the parser, the parser has
grown a dependency it shouldn't have.

## Status: RED, and deliberately not in CI

**8 pages, 98 reference events, 17 differing lines.** It is not wired into
`build.yml`, because a failing job inside a passing suite becomes a failure
everyone learns to ignore. It goes in once the three items below are settled,
and settling them means changing our parser or writing a documented deviation
into `MICRON.md`. **It never means editing the expected output.**

### 1. Heading colours: probably a deviation to document, not a bug

The reference gives headings theme colours, `222222` on `bbbbbb` at depth one
and `111111` on `999999` at depth two. We report `default`.

Those colours come from the **theme**, not from the page. `MICRON.md` says
colour is reported and never resolved, and a theme is the clearest case of
resolution there is. The likely outcome is a stated deviation plus an exclusion
in the differ, but it is a decision, not an oversight, and it stays red until
someone makes it.

### 2. Link targets: a harness gap, not a parser gap

We emit `LINKTARGET`; the reference dump emits none. The `LinkSpec.__init__`
spy doesn't fire when `parse_line` is called with no `url_delegate`, so targets
never reach the dump. The targets our parser produces look right and are
covered by unit tests. **This one is ours to fix in the harness**, by pulling
targets out of the widgets `parse_line` returns.

### 3. A blank line after a literal toggle: a real divergence

On `05-literal-and-escape.mu` the reference emits a line for the `` `= ``
toggle itself and we don't. `Micron.h` documents `onLineEnd` as not firing for
literal toggles, so this is deliberate, but it means **NomadNet renders a blank
line where we render nothing**. Same page, different height. Either we match it
or we write down why we don't.

## Not compared yet

Dividers, fields and anchors. The reference returns those as urwid widgets from
`parse_line` rather than through `make_part`, so the dumper can't see them. Our
side emits them with a `SKIP_` prefix and the differ drops those lines on both
sides, so the gap is visible in the dump instead of silently passing.

## The corpus

Eight pages covering style toggles and nesting, both colour forms, heading
depth and reset, links and anchors, literal blocks and escapes, alignment,
comments and blank lines, and malformed markup. Malformed input matters most:
`MICRON.md` promises a page with a typo loses its formatting and never its
content, and that promise is only worth what a test makes it worth.
