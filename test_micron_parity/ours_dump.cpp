// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dump our parser's callbacks in the same event format reference_dump.py emits,
// so the two can be diffed line for line.
//
// Compiles with nothing but a C++17 compiler and the parser itself:
//   c++ -std=c++17 -I ../lib/Micron/src ours_dump.cpp ../lib/Micron/src/Micron.cpp
// That command is also the cleanest evidence the parser carries no firmware,
// Reticulum or display dependency.

#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <iostream>
#include "Micron.h"

using namespace micron;

static std::string color(const Color& c) {
    if (c.is_default) return "default";
    // Always six hex digits. `F00f and `FTff0000 are the same colour spelled
    // two ways, and the reference dumper widens its side to match.
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06x", c.rgb & 0xffffff);
    return buf;
}

static const char* align_name(Align a) {
    return a == Align::Center ? "center" : a == Align::Right ? "right" : "left";
}

class Dumper : public Renderer {
public:
    void onText(const char* t, size_t n, const Style& s) override {
        if (n == 0) return;
        std::printf("TEXT|%c%c%c|%s|%s|%s|%u|%s|%.*s\n",
                    s.bold ? 'b' : '-', s.italic ? 'i' : '-', s.underline ? 'u' : '-',
                    color(s.fg).c_str(), color(s.bg).c_str(), align_name(s.align),
                    (unsigned)s.depth, s.literal ? "lit" : "-", (int)n, t);
    }
    void onLink(const char* l, size_t ln, const char* t, size_t tn, const Style& s) override {
        // The reference emits a link's LABEL through make_part like any other
        // text, and its TARGET separately through LinkSpec. Match that split so
        // the two dumps line up.
        onText(l, ln, s);
        std::printf("LINKTARGET|%.*s\n", (int)tn, t);
    }
    // Not compared yet: the reference returns these as urwid widgets rather
    // than through make_part, so reference_dump.py cannot see them. Emitted
    // with a SKIP prefix so the differ drops them on this side too, and so the
    // gap is visible in the dump rather than silent.
    void onDivider(uint32_t ch, const Style&) override { std::printf("SKIP_DIV|%u\n", ch); }
    void onField(const Field& f, const Style&) override {
        std::printf("SKIP_FIELD|%.*s\n", (int)f.name_len, f.name ? f.name : "");
    }
    void onAnchor(const char* n, size_t len) override { std::printf("SKIP_ANCHOR|%.*s\n", (int)len, n); }
    void onLineEnd(const Style&) override { std::printf("EOL\n"); }
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ours_dump <page.mu>...\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        std::ifstream in(argv[a]);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[a]); return 1; }
        const char* base = std::strrchr(argv[a], '/');
        std::printf("# page %s\n", base ? base + 1 : argv[a]);

        Parser p;
        p.reset();
        Dumper out;
        std::string line;
        // getline strips the terminator, which is what parseLine expects.
        while (std::getline(in, line)) p.parseLine(line.data(), line.size(), out);
    }
    return 0;
}
