<!-- Copyright (C) 2026 Thicket contributors; GPL-3.0-or-later -->
# Desktop page browser

A window that renders one Nomad Network page at a time with the real renderer,
scrolls it, and follows links between pages. The interactive form of the
render-to-PNG harness: change code, rebuild, browse, no flashing.

It is a backend swap, not a second renderer. The parser, the page renderer and
the 400x240 framebuffer are the device's, called through the one shared
`compose_page()`. Only two edges differ from the handheld:

- **display**: the composed framebuffer is read back into an SDL window scaled
  up, instead of being clocked out over SPI to the Sharp panel.
- **fetch/input**: pages come from local files (and, next, from live nodes over
  the network) rather than from an on-device store, and navigation comes from a
  keyboard rather than the device's buttons.

If the browser rendered a page differently from the device, it would stop being
a faithful test of what the device shows. It does not: the pixels come from the
same code.

## Build and run

Needs SDL2 (`brew install sdl2`) and the pinned micron-cpp, which
`pio test -e native` fetches into `.pio/libdeps/`.

```sh
make            # ./browser, the SDL window
make run        # build and open the bundled demo pageset
make demo       # headless: dump the navigation frames to out/*.png
make headless   # ./browser-headless, --script only, no SDL

./browser path/to/page.mu               # open one page
./browser --root DIR DIR/index.mu       # links resolve against DIR
./browser ~/…/micron-cpp-corpus/tier3-canonical/guide-markup.mu
```

## Keys

| key | action |
|-----|--------|
| arrows, `j`/`k` | scroll a line |
| space, PageUp/Down | scroll a screen |
| Home/End, `g`/`G` | top / bottom |
| Tab / Shift-Tab | focus next / previous link on screen |
| Enter, or click | follow the focused (or clicked) link |
| Backspace, Left, `b` | back |
| `q`, Esc | quit |

The focused link is drawn reverse-video, which is how the reference
distinguishes a selected link. A link is only focusable while it is on screen,
because the renderer records a link's hit-box only when it lays it inside the
scroll window, exactly as the device hit-tests only what it can show. Scroll,
then Tab reaches the links further down.

## Link resolution

A Micron link target is `<destination-hash>:/page/<path>`. Locally, that maps to
the frozen corpus naming `<hash16>__page__<path>.mu`; plain relative targets
(`about.mu`, `/page/x.mu`) resolve against the current page's directory and the
`--root`. A target with no local page reports "not held locally"; fetching it
over the network is the next piece. In-page anchors (`:name`) are not yet
followed.

## Headless capture

`--script` runs a `;`-separated command list and writes a PBM frame per `shot`,
which `scripts/pbm2png.py` turns into PNG. Commands: `down N`, `up N`,
`pagedown`, `pageup`, `top`, `bottom`, `next`, `prev`, `focus N`, `enter`,
`back`, `open PATH`, `shot`. It never initializes SDL, so it runs without a
display and doubles as a scriptable render test.

## Not this, yet

No message list, no menus, no device chrome: the page browser alone. The full
device-UI simulator can grow from here later. For now it fetches a page, renders
it, scrolls it, and follows links.
