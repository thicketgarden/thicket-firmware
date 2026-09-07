# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Desktop page-browser target. Bare compiler, not PlatformIO: this is the
# interactive form of the render harness and the loop is change-rebuild-browse,
# which a 75-second PlatformIO cycle is not.
#
#   make            build ./browser (needs SDL2)
#   make headless   build ./browser-headless (no SDL, --script only)
#   make demo       build and dump the offline navigation frames to out/
#   make run        build and open the demo pageset in a window
#   make clean

REPO := ..
MC   := $(REPO)/.pio/libdeps/native/micron-cpp/src
SRC  := $(REPO)/lib/MicronRender/src
LCD  := $(REPO)/lib/SharpLcd/src

INCS := -I$(SRC) -I$(LCD) -I$(MC)
CORE := browser.cpp \
        $(SRC)/MicronRender.cpp \
        $(LCD)/SharpLcd.cpp \
        $(LCD)/VirtualPanel.cpp \
        $(MC)/Micron.cpp
CXXFLAGS := -std=c++17 -O1 -Wall -Wextra $(INCS)

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

.PHONY: all headless demo run clean check-deps

all: check-deps browser

check-deps:
	@test -d "$(MC)" || { \
	  echo "[desktop] micron-cpp not fetched. Run once from the repo root:"; \
	  echo "[desktop]   pio test -e native"; exit 1; }

browser: check-deps $(CORE)
	@test -n "$(SDL_LIBS)" || { echo "[desktop] SDL2 not found (brew install sdl2), or 'make headless'"; exit 1; }
	c++ $(CXXFLAGS) $(SDL_CFLAGS) -o $@ $(CORE) $(SDL_LIBS)
	@echo "[desktop] built ./browser"

browser-headless: check-deps $(CORE)
	c++ $(CXXFLAGS) -DTHICKET_NO_SDL -o $@ $(CORE)
	@echo "[desktop] built ./browser-headless (--script only)"

headless: browser-headless

run: browser
	./browser pages

# Offline navigation proof: index -> focus a link -> follow -> scroll -> back,
# then a corpus page scrolled to its content. Frames land in out/ as PNG.
demo: browser-headless
	@mkdir -p out
	./browser-headless --root pages pages/index.mu --out out --script \
	  'shot; pagedown; shot; next; shot; enter; shot; back; shot'
	@for f in out/*.pbm; do python3 $(REPO)/scripts/pbm2png.py $$f $${f%.pbm}.png >/dev/null; done
	@echo "[desktop] navigation frames in out/*.png"

clean:
	rm -f browser browser-headless out/*.pbm out/*.png
