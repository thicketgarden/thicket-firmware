// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parser lives in thicketgarden/micron-cpp now, with its own 36 unit tests
// and a parity harness against NomadNet's own parser. None of that is repeated
// here.
//
// This tests the one thing that can only break in THIS repository: that the
// pinned dependency resolves, compiles, and parses. A bad pin is the new
// failure mode the move introduced, and it would otherwise surface as a link
// error in whatever lands next rather than as a named test.

#include <unity.h>
#include <string>
#include <cstring>
#include "Micron.h"

using namespace micron;

namespace {
class Probe : public Renderer {
public:
    std::string text;
    std::string target;
    bool bold_seen = false;
    int lines = 0;

    void onText(const char* t, size_t n, const Style& s) override {
        text.append(t, n);
        if (s.bold) bold_seen = true;
    }
    void onLink(const char*, size_t, const char* t, size_t tn, const Style&) override {
        target.assign(t, tn);
    }
    void onDivider(uint32_t, const Style&) override {}
    void onField(const Field&, const Style&) override {}
    void onAnchor(const char*, size_t) override {}
    void onLineEnd(const Style&) override { lines++; }
};

void feed(Parser& p, Probe& r, const char* line) { p.parseLine(line, strlen(line), r); }
} // namespace

void test_dependency_parses_styled_text() {
    Parser p; Probe r;
    feed(p, r, "`!bold`! plain");
    TEST_ASSERT_EQUAL_STRING("bold plain", r.text.c_str());
    TEST_ASSERT_TRUE(r.bold_seen);
    TEST_ASSERT_EQUAL_INT(1, r.lines);
}

void test_dependency_parses_a_link() {
    Parser p; Probe r;
    feed(p, r, "`[Home`:/page/index.mu]");
    TEST_ASSERT_EQUAL_STRING(":/page/index.mu", r.target.c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dependency_parses_styled_text);
    RUN_TEST(test_dependency_parses_a_link);
    return UNITY_END();
}
