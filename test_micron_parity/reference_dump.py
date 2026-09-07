#!/usr/bin/env python3
"""Dump NomadNet's own MicronParser output as comparable events.

The reference is driven, never reimplemented. parse_line() is called for every
line of every corpus page and its internal make_part() is intercepted, so what
comes out is the reference's own styled spans rather than our reading of them.

NomadNet's parser reaches for the running application in four places, all of
them rendering concerns: a theme, an urwid screen to register palette entries
against, and a colour mode. A stub supplies those and nothing else. No parsing
logic is patched. Colour mode is COLORMODE_TRUE so the reference does not
quantise colour to a terminal palette, which is the closest match to a parser
that reports colour as the page asked for it.

  ./reference_dump.py corpus/*.mu > reference.events

Compared today: TEXT spans with full style, and link labels and targets.
NOT compared: dividers, fields and anchors. The reference returns those as
urwid widgets from parse_line rather than through make_part, so they need a
second extraction path that does not exist yet. Our side emits them; this side
does not, and the differ skips those classes on both sides rather than
pretending they matched.
"""

import copy, sys, os

import nomadnet, urwid
import nomadnet.ui.TextUI as T


class _Screen:
    """urwid needs a real screen object to register palette entries against."""
    def __init__(self):
        self._s = urwid.raw_display.Screen()
    def __getattr__(self, k):
        return getattr(self._s, k)


class _UI:
    screen = _Screen()
    colormode = T.COLORMODE_TRUE


class _App:
    config = {"textui": {"theme": T.THEME_DARK}}
    ui = _UI()


_APP = _App()
nomadnet.NomadNetworkApp.get_shared_instance = staticmethod(lambda: _APP)

from nomadnet.ui.textui import MicronParser as M  # noqa: E402  (needs the stub first)

_parts = []
_links = []
_orig_make_part = M.make_part
_orig_linkspec_init = M.LinkSpec.__init__


def _spy_make_part(state, part):
    _parts.append((copy.deepcopy(state), part))
    return _orig_make_part(state, part)


def _spy_linkspec(self, link_target, orig_spec, cm=256):
    _links.append(link_target)
    return _orig_linkspec_init(self, link_target, orig_spec, cm)


M.make_part = _spy_make_part
M.LinkSpec.__init__ = _spy_linkspec


def _color(value, default):
    """Normalise to six hex digits so `F00f and `FTff0000 compare equal.

    Micron spells the same colour two ways and the reference keeps the
    spelling. A renderer cares about the colour, not which form the page used,
    so both sides are widened here rather than our parser being asked to carry
    a distinction it has no use for."""
    if value is None or value == default:
        return "default"
    v = str(value)
    if len(v) == 3:
        v = "".join(c * 2 for c in v)
    return v.lower()


def dump(path, out):
    state = M.default_state()
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    # A file ending in a newline is not a file with a trailing blank line.
    if text.endswith("\n"):
        text = text[:-1]
    lines = text.split("\n")

    print(f"# page {os.path.basename(path)}", file=out)
    for lineno, line in enumerate(lines, 1):
        _parts.clear()
        _links.clear()
        try:
            M.parse_line(line, state, None)
        except Exception as e:                       # noqa: BLE001
            print(f"REFERENCE_ERROR|{lineno}|{type(e).__name__}: {e}", file=out)
            continue

        for st, part in _parts:
            if part == "":
                continue
            f = st["formatting"]
            flags = ("b" if f["bold"] else "-") + ("i" if f["italic"] else "-") \
                  + ("u" if f["underline"] else "-")
            print("TEXT|{}|{}|{}|{}|{}|{}|{}".format(
                flags,
                _color(st["fg_color"], st["default_fg"]),
                _color(st["bg_color"], st["default_bg"]),
                st["align"], st["depth"],
                "lit" if st["literal"] else "-",
                part), file=out)
        for target in _links:
            print(f"LINKTARGET|{target}", file=out)
        print("EOL", file=out)


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    for path in argv[1:]:
        dump(path, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
