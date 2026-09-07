// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thicket page browser, desktop target.
//
// A window that renders ONE Nomad Network page at a time with the real
// renderer, scrolls it, and follows links between pages. No message list, no
// menus, no device chrome: the interactive form of the render-to-PNG harness.
//
// The renderer, the parser and the framebuffer are the device's, unchanged and
// unforked. compose_page() lays the page into a 400x240 1-bit framebuffer
// exactly as it does on the handheld; the only thing that differs here is the
// display backend. Instead of clocking the framebuffer out over SPI to the
// Sharp panel, this reads it back and blits it into an SDL window, scaled up so
// a person can see it. If the browser ever diverged from the device it would
// stop being a faithful test of what the device shows, so it does not: it is a
// backend swap under the real renderer, not a parallel renderer.
//
// Two front-ends over one core:
//   interactive   an SDL window with keyboard and mouse (the Mac dev loop).
//   --script      a headless driver that dumps PBM frames, for proving the
//                 thing works without a display. Never touches SDL video.
//
// Milestone 1 is local files only: open a .mu file or a directory of them,
// scroll, and follow links that resolve to a page on disk. Live RNS fetch is
// the fetch edge and lands next; a link with no local page reports that here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "Micron.h"
#include "SharpLcd.h"
#include "VirtualPanel.h"
#include "MicronRender.h"
#include "ComposePage.h"

using namespace thicket;

// ---------------------------------------------------------------------------
// The panel framebuffer. The single copy of the screen, same as on the device.
static uint8_t g_fb[LCD_FB_BYTES];

// A bus that goes nowhere. get_pixel()/set_pixel() read and write the
// framebuffer directly, so the browser never needs a real transport; flush()
// hands its bytes to this and they are dropped.
class NullBus : public SharpLcdBus {
public:
	void select(bool) override {}
	void write(const uint8_t*, size_t) override {}
};

// A link as the browser holds it: panel rectangle plus a stable copy of the
// target string, because LinkBox::target points into page source that a later
// navigation will free.
struct Box {
	int x, y, w, h;
	std::string target;
};

struct Frame {
	uint16_t height = 0;
	std::vector<Box> links;
};

// Lay `src` into g_fb at `scroll` and return its height and on-screen links.
// Renderer flags are left at their constructor defaults, which are the device's
// shipped defaults, so the window shows the device's layout and not the
// harness's leading-0 comparison layout.
static Frame compose(const std::string& src, uint16_t scroll) {
	NullBus bus;
	SharpLcd lcd(bus, g_fb);
	PageRenderer r(lcd);
	Frame f;
	f.height = compose_page(lcd, r, src.data(), src.size(), scroll);
	for (uint8_t i = 0; i < r.link_count(); ++i) {
		const LinkBox& b = r.link(i);
		f.links.push_back({b.x, b.y, b.w, b.h,
		                   std::string(b.target, b.target_len)});
	}
	return f;
}

// Reverse-video the focused link, drawn straight into the framebuffer. This is
// the device's own focus affordance, previewed at the display edge without
// touching the renderer: the reference distinguishes the selected link by
// inverting it, and so do we.
static void invert_box(const Box& b) {
	NullBus bus;
	SharpLcd lcd(bus, g_fb);
	for (int y = b.y; y < b.y + b.h; ++y) {
		if (y < 0 || y >= LCD_HEIGHT) continue;
		for (int x = b.x - 1; x < b.x + b.w + 1; ++x) {
			if (x < 0 || x >= LCD_WIDTH) continue;
			lcd.set_pixel((uint16_t)x, (uint16_t)y,
			              !lcd.get_pixel((uint16_t)x, (uint16_t)y));
		}
	}
}

// ---------------------------------------------------------------------------
// Small filesystem + string helpers.

static bool file_exists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}
static bool is_dir(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}
static std::string dirname_of(const std::string& p) {
	auto s = p.find_last_of('/');
	return s == std::string::npos ? std::string(".") : p.substr(0, s);
}
static std::string basename_of(const std::string& p) {
	auto s = p.find_last_of('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}
static bool read_file(const std::string& path, std::string& out) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	char buf[65536];
	size_t n;
	out.clear();
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
	std::fclose(f);
	return true;
}
static bool looks_hex(const std::string& s) {
	if (s.size() < 8) return false;
	for (char c : s)
		if (!std::isxdigit((unsigned char)c)) return false;
	return true;
}
static std::string replace_all(std::string s, const std::string& a, const std::string& b) {
	for (size_t i = 0; (i = s.find(a, i)) != std::string::npos; i += b.size())
		s.replace(i, a.size(), b);
	return s;
}

// Resolve a Micron link target to a local file, or "" if none is held.
//
// Targets are `<destination-hash>:/page/<path>` on the mesh; the frozen corpus
// names those files `<hash16>__page__<path with / as __>.mu`. Plain relative
// targets (`about.mu`, `/page/x.mu`) resolve against the current page's
// directory and the root. An in-page anchor (`:name`) is not a file and returns
// "".
static std::string resolve(const std::string& target,
                           const std::string& curdir,
                           const std::string& root) {
	if (target.empty() || target[0] == ':') return "";

	std::string hash, path;
	auto c = target.find(':');
	if (c != std::string::npos && looks_hex(target.substr(0, c))) {
		hash = target.substr(0, c);
		path = target.substr(c + 1);
	} else {
		path = target;
	}

	std::string rel = path;
	if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);   // page/index.mu
	const std::string und = replace_all(rel, "/", "__");  // page__index.mu
	const std::string base = basename_of(path);

	std::vector<std::string> cands;
	if (!hash.empty()) {
		cands.push_back(root + "/" + hash.substr(0, 16) + "__" + und);
		cands.push_back(root + "/" + hash + "__" + und);
	}
	cands.push_back(curdir + "/" + base);
	cands.push_back(curdir + "/" + rel);
	cands.push_back(root + "/" + rel);
	cands.push_back(root + "/" + base);

	for (const auto& cand : cands) {
		if (file_exists(cand)) return cand;
		if (cand.size() < 3 || cand.substr(cand.size() - 3) != ".mu") {
			std::string mu = cand + ".mu";
			if (file_exists(mu)) return mu;
		}
	}
	return "";
}

// ---------------------------------------------------------------------------
// Browser state: the current page, a back stack, scroll and link focus.

static uint16_t clamp_scroll(long s, uint16_t height) {
	long maxs = (long)height - LCD_HEIGHT;
	if (maxs < 0) maxs = 0;
	if (s < 0) s = 0;
	if (s > maxs) s = maxs;
	return (uint16_t)s;
}

struct PageState {
	std::string src;
	std::string address;   // what to show the reader: a path or a mesh target
	std::string dir;       // directory to resolve relative links against
	uint16_t scroll = 0;
	int focus = -1;        // index into the last frame's links, or -1
};

class Browser {
public:
	Browser(const std::string& root) : _root(root) {}

	const char* status() const { return _status.c_str(); }
	const PageState& page() const { return _cur; }

	bool open_path(const std::string& path) {
		std::string src;
		if (!read_file(path, src)) {
			_status = "cannot open " + path;
			return false;
		}
		_cur = PageState{};
		_cur.src = std::move(src);
		_cur.address = path;
		_cur.dir = dirname_of(path);
		_status = "opened " + basename_of(path);
		recompose();
		return true;
	}

	// The frame for the current state, focus overlay applied. Recomputed on any
	// change; a re-flow is well under a millisecond, so there is no layout cache
	// to keep in sync, matching the renderer's no-layout-tree design.
	const Frame& recompose() {
		_frame = compose(_cur.src, _cur.scroll);
		_cur.scroll = clamp_scroll(_cur.scroll, _frame.height);
		if (_cur.scroll != _last_scroll) {
			_frame = compose(_cur.src, _cur.scroll);
			_last_scroll = _cur.scroll;
		}
		if (_cur.focus >= (int)_frame.links.size()) _cur.focus = -1;
		if (_cur.focus >= 0) invert_box(_frame.links[_cur.focus]);
		return _frame;
	}

	void scroll_by(long d) {
		_cur.scroll = clamp_scroll((long)_cur.scroll + d, _frame.height);
		_last_scroll = _cur.scroll;
		_cur.focus = -1;
	}
	void scroll_top() { _cur.scroll = 0; _last_scroll = 0; _cur.focus = -1; }
	void scroll_bottom() { scroll_by(_frame.height); }

	// Cycle the links currently on screen. dir +1 next, -1 previous.
	void focus_cycle(int dir) {
		int n = (int)_frame.links.size();
		if (n == 0) { _status = "no links on screen; scroll to more"; _cur.focus = -1; return; }
		if (_cur.focus < 0) _cur.focus = dir > 0 ? 0 : n - 1;
		else _cur.focus = (_cur.focus + dir + n) % n;
		_status = "link " + std::to_string(_cur.focus + 1) + "/" +
		          std::to_string(n) + ": " + _frame.links[_cur.focus].target;
	}
	void set_focus(int i) {
		if (i >= 0 && i < (int)_frame.links.size()) _cur.focus = i;
	}

	// Follow the focused link, or a hit-tested one. Pushes the current page so
	// Back returns to it at the same scroll.
	bool follow_focus() {
		if (_cur.focus < 0 || _cur.focus >= (int)_frame.links.size()) {
			_status = "no link focused";
			return false;
		}
		return follow_target(_frame.links[_cur.focus].target);
	}
	bool click_at(int panel_x, int panel_y) {
		for (size_t i = 0; i < _frame.links.size(); ++i) {
			const Box& b = _frame.links[i];
			if (panel_x >= b.x && panel_x < b.x + b.w &&
			    panel_y >= b.y && panel_y < b.y + b.h) {
				_cur.focus = (int)i;
				return follow_target(b.target);
			}
		}
		return false;
	}
	bool follow_target(const std::string& target) {
		std::string path = resolve(target, _cur.dir, _root);
		if (path.empty()) {
			// The fetch edge lands here next: a target with no local page is
			// exactly what the network path is for.
			_status = "not held locally: " + target;
			return false;
		}
		std::string src;
		if (!read_file(path, src)) { _status = "cannot open " + path; return false; }
		_cur.focus = -1;
		_stack.push_back(_cur);
		_cur = PageState{};
		_cur.src = std::move(src);
		_cur.address = target;
		_cur.dir = dirname_of(path);
		_last_scroll = 0;
		_status = "followed -> " + basename_of(path);
		return true;
	}
	bool back() {
		if (_stack.empty()) { _status = "no page to go back to"; return false; }
		_cur = _stack.back();
		_stack.pop_back();
		_last_scroll = _cur.scroll;
		_status = "back -> " + basename_of(_cur.address);
		return true;
	}

	uint16_t height() const { return _frame.height; }
	int scroll_pct() const {
		long span = (long)_frame.height - LCD_HEIGHT;
		if (span <= 0) return 100;
		return (int)((100L * _cur.scroll) / span);
	}

private:
	std::string _root;
	PageState _cur;
	Frame _frame;
	std::vector<PageState> _stack;
	uint16_t _last_scroll = 0;
	std::string _status;
};

// ---------------------------------------------------------------------------
// Framebuffer readout.

// P1 PBM straight from g_fb, matching VirtualPanel's format so scripts/pbm2png
// converts a browser frame the same way it converts a harness one.
static bool write_pbm(const std::string& path, uint8_t scale) {
	if (scale == 0) scale = 1;
	NullBus bus; SharpLcd lcd(bus, g_fb);
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	std::fprintf(f, "P1\n# thicket browser frame\n%u %u\n",
	             (unsigned)(LCD_WIDTH * scale), (unsigned)(LCD_HEIGHT * scale));
	for (uint16_t y = 0; y < LCD_HEIGHT; ++y)
		for (uint8_t sy = 0; sy < scale; ++sy) {
			for (uint16_t x = 0; x < LCD_WIDTH; ++x) {
				char c = lcd.get_pixel(x, y) ? '1' : '0';
				for (uint8_t sx = 0; sx < scale; ++sx) std::fputc(c, f);
			}
			std::fputc('\n', f);
		}
	std::fclose(f);
	return true;
}

// ===========================================================================
// Headless script driver. No SDL: composes states and writes PBM frames.
//
// Commands, ';'-separated: down N | up N | pagedown | pageup | top | bottom |
// next | prev | focus N | enter | back | open PATH | shot. Every `shot` writes
// the next NN.pbm into the out directory.

static int run_script(Browser& br, const std::string& script,
                      const std::string& outdir, uint8_t scale) {
	int shot = 0;
	size_t i = 0;
	while (i < script.size()) {
		size_t e = script.find(';', i);
		if (e == std::string::npos) e = script.size();
		std::string cmd = script.substr(i, e - i);
		i = e + 1;
		// trim
		while (!cmd.empty() && std::isspace((unsigned char)cmd.front())) cmd.erase(cmd.begin());
		while (!cmd.empty() && std::isspace((unsigned char)cmd.back())) cmd.pop_back();
		if (cmd.empty()) continue;

		std::string op = cmd;
		long arg = 0;
		auto sp = cmd.find(' ');
		if (sp != std::string::npos) { op = cmd.substr(0, sp); arg = std::atol(cmd.c_str() + sp + 1); }

		if      (op == "down")     br.scroll_by(arg ? arg : 15);
		else if (op == "up")       br.scroll_by(-(arg ? arg : 15));
		else if (op == "pagedown") br.scroll_by(LCD_HEIGHT - 20);
		else if (op == "pageup")   br.scroll_by(-(LCD_HEIGHT - 20));
		else if (op == "top")      br.scroll_top();
		else if (op == "bottom")   br.scroll_bottom();
		else if (op == "next")     br.focus_cycle(+1);
		else if (op == "prev")     br.focus_cycle(-1);
		else if (op == "focus")    br.set_focus((int)arg);
		else if (op == "enter")    br.follow_focus();
		else if (op == "back")     br.back();
		else if (op == "open")     br.open_path(cmd.substr(sp + 1));
		else if (op == "shot") {
			br.recompose();
			char name[64];
			std::snprintf(name, sizeof name, "%s/%02d.pbm", outdir.c_str(), shot++);
			write_pbm(name, scale);
			std::printf("[shot %02d] %-28s h=%u %d%%  %s\n",
			            shot - 1, br.page().address.c_str(), br.height(),
			            br.scroll_pct(), br.status());
			continue;
		} else {
			std::fprintf(stderr, "unknown command: %s\n", op.c_str());
		}
		br.recompose();
	}
	std::printf("[script] wrote %d frame(s) to %s\n", shot, outdir.c_str());
	return 0;
}

// ===========================================================================
#ifndef THICKET_NO_SDL
#include <SDL2/SDL.h>

static void blit(SDL_Texture* tex) {
	NullBus bus; SharpLcd lcd(bus, g_fb);
	void* pixels; int pitch;
	SDL_LockTexture(tex, nullptr, &pixels, &pitch);
	// Sharp memory LCD palette: paper ground, near-black ink.
	const uint32_t ink = 0xFF1A1A18, paper = 0xFFE9E9E1;
	for (int y = 0; y < LCD_HEIGHT; ++y) {
		uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
		for (int x = 0; x < LCD_WIDTH; ++x)
			row[x] = lcd.get_pixel((uint16_t)x, (uint16_t)y) ? ink : paper;
	}
	SDL_UnlockTexture(tex);
}

static void set_title(SDL_Window* w, Browser& br) {
	char t[512];
	std::snprintf(t, sizeof t, "Thicket browser  -  %s  -  %d%%  |  %s",
	              br.page().address.c_str(), br.scroll_pct(), br.status());
	SDL_SetWindowTitle(w, t);
}

static int run_interactive(Browser& br, int scale) {
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* win = SDL_CreateWindow("Thicket browser",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		LCD_WIDTH * scale, LCD_HEIGHT * scale, SDL_WINDOW_ALLOW_HIGHDPI);
	SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, LCD_WIDTH, LCD_HEIGHT);
	SDL_RenderSetLogicalSize(ren, LCD_WIDTH, LCD_HEIGHT);   // nearest-neighbour scale

	const int LINE = 15, PAGE = LCD_HEIGHT - 20;
	bool running = true, dirty = true;
	while (running) {
		SDL_Event ev;
		while (SDL_WaitEvent(&ev)) {
			bool handled = true;
			if (ev.type == SDL_QUIT) { running = false; }
			else if (ev.type == SDL_KEYDOWN) {
				bool shift = (ev.key.keysym.mod & KMOD_SHIFT);
				switch (ev.key.keysym.sym) {
					case SDLK_q: case SDLK_ESCAPE: running = false; break;
					case SDLK_DOWN: case SDLK_j: br.scroll_by(LINE); break;
					case SDLK_UP:   case SDLK_k: br.scroll_by(-LINE); break;
					case SDLK_PAGEDOWN: br.scroll_by(PAGE); break;
					case SDLK_PAGEUP:   br.scroll_by(-PAGE); break;
					case SDLK_SPACE: br.scroll_by(shift ? -PAGE : PAGE); break;
					case SDLK_HOME: br.scroll_top(); break;
					case SDLK_END: br.scroll_bottom(); break;
					case SDLK_g: shift ? br.scroll_bottom() : br.scroll_top(); break;
					case SDLK_TAB: br.focus_cycle(shift ? -1 : +1); break;
					case SDLK_RETURN: case SDLK_RETURN2: br.follow_focus(); break;
					case SDLK_LEFT: case SDLK_BACKSPACE: case SDLK_b: br.back(); break;
					default: handled = false; break;
				}
			}
			else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
				int mx, my; SDL_GetMouseState(&mx, &my);
				float lx, ly; SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
				br.click_at((int)lx, (int)ly);
			}
			else handled = false;
			if (handled) { dirty = true; break; }
		}
		if (!running) break;
		if (dirty) {
			br.recompose();
			blit(tex);
			set_title(win, br);
			dirty = false;
		}
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, nullptr, nullptr);
		SDL_RenderPresent(ren);
	}
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
#endif  // THICKET_NO_SDL

// ===========================================================================
static void usage() {
	std::fprintf(stderr,
	    "usage: browser [--root DIR] [--scale N] PAGE.mu\n"
	    "       browser [--root DIR] [--scale N] --script 'cmds' --out DIR PAGE.mu\n"
	    "\n"
	    "  PAGE.mu    a Micron page, or a directory (its index.mu is opened).\n"
	    "  --root     directory link targets resolve against (default: PAGE's dir).\n"
	    "  --scale    window pixel scale, interactive only (default 3).\n"
	    "  --script   headless: run ';'-separated commands, dumping PBM frames.\n"
	    "  --out      where --script writes frames (default: current dir).\n"
	    "\n"
	    "keys: arrows/jk scroll, space page, Home/End, Tab focus link,\n"
	    "      Enter follow, Backspace/Left back, q quit.\n");
}

int main(int argc, char** argv) {
	std::string root, page, script, outdir = ".";
	int scale = 3;
	(void)scale;
	bool have_script = false;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if      (a == "--root"   && i + 1 < argc) root = argv[++i];
		else if (a == "--scale"  && i + 1 < argc) scale = std::atoi(argv[++i]);
		else if (a == "--script" && i + 1 < argc) { script = argv[++i]; have_script = true; }
		else if (a == "--out"    && i + 1 < argc) outdir = argv[++i];
		else if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (!a.empty() && a[0] == '-') { usage(); return 2; }
		else page = a;
	}
	if (page.empty()) { usage(); return 2; }

	// A directory argument opens its index.mu and resolves links against it.
	if (is_dir(page)) {
		if (root.empty()) root = page;
		page = page + "/index.mu";
	}
	if (root.empty()) root = dirname_of(page);

	Browser br(root);
	if (!br.open_path(page)) {
		std::fprintf(stderr, "%s\n", br.status());
		return 1;
	}

	if (have_script) return run_script(br, script, outdir, 1);

#ifdef THICKET_NO_SDL
	std::fprintf(stderr, "built without SDL; use --script for headless frames.\n");
	return 2;
#else
	return run_interactive(br, scale < 1 ? 1 : scale);
#endif
}
