# Micron, as implemented here

Reference: `markqvist/NomadNet`, `nomadnet/ui/textui/MicronParser.py` (1,048
lines), read 2026-08-01. **That file is the grammar of record.** This document
exists because a careful second-hand summary of it was wrong in five places,
and every correction below is a passing test in `test/test_micron/`.

## Line level

| Construct | Meaning |
|---|---|
| `` `= `` | **toggles a literal block.** Inside one, markup is emitted verbatim; `` \`= `` escapes a literal `` `= `` |
| `#` | comment, whole line dropped |
| `>` `>>` `>>>` | section heading; the count sets depth, which persists across lines. Content indents `depth * 2` |
| `<` | resets depth to 0, then re-parses the rest of the line |
| `-` | horizontal divider. `-x` sets the fill character; default U+2500. Control characters are rejected |
| `\` | leading backslash makes the first character literal, which is how a line beginning `>` or `-` stays text |
| `` `t `` | table open/close, optional `l`/`c`/`r` and a max width. **Not implemented** |
| `` `{ `` | in-page partial. **Not implemented** |

**Heading sanitisation:** a `>` line containing a field (`` `< ``) loses its
heading status, because a heading style cannot wrap an editable widget.

## Inline, after a backtick

| | |
|---|---|
| `_` `!` `*` | toggle underline, bold, italic |
| `` ` `` | **reset every attribute** (not a literal toggle) |
| `F<rgb>` / `B<rgb>` | foreground / background, 3 nibbles doubled: `f00` becomes `#ff0000` |
| `FT<rrggbb>` / `BT<rrggbb>` | **true colour, six hex digits** |
| `f` / `b` | foreground / background back to default |
| `c` `l` `r` `a` | centre, left, right, default alignment |
| `:name` | zero-width **anchor** for in-document links |
| `` [label`target] `` | link; with no backtick the whole body is both label and target |
| `<flags\|name`value>` | field. Flags: `^` radio, `?` checkbox, `!` masked, digits set width (default 24) |

## The five things the summary got wrong

Recorded because they are the reason this file exists, not as trivia.

1. `` `= `` is the **literal toggle**, not a divider.
2. A lone backtick is a **style reset**, not the literal toggle.
3. The **divider is `-`** at line start, with an optional fill character.
4. **`FT` / `BT` true-colour forms exist** and were missing entirely.
5. **Checkbox and radio widgets exist**, not just text fields. Tables and
   anchors were omitted too.

## Deliberate deviations

- **Malformed markup drops the marker and keeps the words.** An unterminated
  link or field emits its text rather than swallowing it. A page with a typo
  should lose its formatting, never its content.
- **Unknown commands are consumed**, so a stray backtick does not leak.
- **Colour is reported, never resolved.** The parser hands the renderer what
  the page asked for. Deciding what `` `F00f `` means on a two-ink panel is the
  renderer's problem, and on a two-ink panel it is still an open design
  question here.

## Not implemented

Tables and partials, both skipped silently rather than emitted as raw markup.
A visible `` `t `` would be worse than a missing table.
