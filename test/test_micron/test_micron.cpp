// Copyright (C) 2026 Thicket contributors
// GPL-3.0-or-later.
//
// Unit tests for the Micron parser, run on the host: `pio test -e native`.
//
// Cases are taken from markqvist/NomadNet's MicronParser.py behaviour, and
// several exist specifically because an earlier second-hand summary of the
// grammar was wrong about them. Those are marked FIXES-SUMMARY.

#include <unity.h>
#include <string>
#include <vector>
#include "Micron.h"

using namespace micron;

// A renderer that records every callback as a readable string, so a test can
// assert on the whole event stream rather than on internal state.
class Recorder : public Renderer {
public:
    std::vector<std::string> events;

    void onText(const char* t, size_t n, const Style& s) override {
        std::string e = "TEXT[" + std::string(t, n) + "]";
        if (s.bold)      e += "+b";
        if (s.italic)    e += "+i";
        if (s.underline) e += "+u";
        if (!s.fg.is_default) e += "+fg" + hex(s.fg.rgb);
        if (!s.bg.is_default) e += "+bg" + hex(s.bg.rgb);
        if (s.align == Align::Center) e += "+center";
        if (s.align == Align::Right)  e += "+right";
        if (s.depth) e += "+d" + std::to_string(s.depth);
        if (s.literal) e += "+lit";
        events.push_back(e);
    }
    void onLink(const char* l, size_t ln, const char* t, size_t tn, const Style&) override {
        events.push_back("LINK[" + std::string(l, ln) + "->" + std::string(t, tn) + "]");
    }
    void onDivider(uint32_t ch, const Style& s) override {
        events.push_back("DIV[" + std::to_string(ch) + "]"
                         + (s.depth ? "+d" + std::to_string(s.depth) : ""));
    }
    void onField(const Field& f, const Style&) override {
        std::string k = f.kind == FieldKind::Radio ? "radio"
                      : f.kind == FieldKind::Checkbox ? "check" : "text";
        events.push_back("FIELD[" + std::string(f.name, f.name_len) + "="
                         + std::string(f.value, f.value_len) + ",w" + std::to_string(f.width)
                         + "," + k + (f.masked ? ",masked" : "") + "]");
    }
    void onAnchor(const char* n, size_t len) override {
        events.push_back("ANCHOR[" + std::string(n, len) + "]");
    }
    void onLineEnd(const Style&) override { events.push_back("EOL"); }

    std::string joined() const {
        std::string r;
        for (size_t i = 0; i < events.size(); i++) { if (i) r += " "; r += events[i]; }
        return r;
    }
private:
    static std::string hex(uint32_t v) {
        const char* d = "0123456789abcdef";
        std::string s;
        for (int i = 20; i >= 0; i -= 4) s += d[(v >> i) & 0xf];
        return s;
    }
};

static std::string runLines(const std::vector<std::string>& lines) {
    Parser p;
    Recorder r;
    for (const auto& l : lines) p.parseLine(l.c_str(), l.size(), r);
    return r.joined();
}
static std::string run(const std::string& line) { return runLines(std::vector<std::string>{line}); }

// --- plain text -------------------------------------------------------------

void test_plain_text(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[hello world] EOL", run("hello world").c_str());
}

void test_empty_line_emits_nothing(void) {
    TEST_ASSERT_EQUAL_STRING("", run("").c_str());
}

void test_comment_emits_nothing(void) {
    TEST_ASSERT_EQUAL_STRING("", run("# a comment").c_str());
}

// --- inline formatting ------------------------------------------------------

void test_bold_toggles(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[a] TEXT[b]+b TEXT[c] EOL", run("a`!b`!c").c_str());
}

void test_italic_and_underline(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[x]+i+u EOL", run("`*`_x").c_str());
}

void test_backtick_resets_all(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[a]+b TEXT[b] EOL", run("`!a``b").c_str());
}

// FIXES-SUMMARY: a lone backtick is a style RESET, not a literal-block toggle.
void test_lone_backtick_is_reset_not_literal(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[after] EOL", run("``after").c_str());
}

// --- colour -----------------------------------------------------------------

void test_short_colour_doubles_nibbles(void) {
    // `Ff00 -> #ff0000
    TEST_ASSERT_EQUAL_STRING("TEXT[red]+fgff0000 EOL", run("`Ff00red").c_str());
}

// FIXES-SUMMARY: the `FT / `BT six-hex true-colour form was missing entirely.
void test_true_colour_form(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[x]+fg123456 EOL", run("`FT123456x").c_str());
}

void test_background_and_reset(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[a]+bg00ff00 TEXT[b] EOL", run("`B0f0a`bb").c_str());
}

void test_malformed_colour_is_dropped_not_printed(void) {
    // "zz" is not hex; the command is consumed and no colour applied.
    TEST_ASSERT_EQUAL_STRING("TEXT[zzq] EOL", run("`Fzzq").c_str());
}

// --- links ------------------------------------------------------------------

void test_link_with_label(void) {
    TEST_ASSERT_EQUAL_STRING("LINK[Home->:/page/index.mu] EOL",
                             run("`[Home`:/page/index.mu]").c_str());
}

void test_link_without_label_uses_target(void) {
    TEST_ASSERT_EQUAL_STRING("LINK[/page/a.mu->/page/a.mu] EOL",
                             run("`[/page/a.mu]").c_str());
}

void test_text_around_link(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[go ] LINK[here->x] TEXT[ now] EOL",
                             run("go `[here`x] now").c_str());
}

// --- headings and sections --------------------------------------------------

void test_heading_sets_depth(void) {
    // MicronParser strips only the ">" characters (line = line[depth:]),
    // never the space after them, so the leading space is content.
    TEST_ASSERT_EQUAL_STRING("TEXT[ Title]+d1 EOL", run("> Title").c_str());
}

void test_deeper_heading(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[Sub]+d3 EOL", run(">>>Sub").c_str());
}

void test_depth_persists_then_resets(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[H]+d2 EOL TEXT[body]+d2 EOL TEXT[flat] EOL",
                             runLines({">>H", "body", "<flat"}).c_str());
}

// --- dividers ---------------------------------------------------------------

// FIXES-SUMMARY: the divider is a line starting with '-'. The summary had it
// as `= , which is actually the literal toggle.
void test_divider_default_char(void) {
    TEST_ASSERT_EQUAL_STRING("DIV[9472]", run("-").c_str());   // U+2500
}

void test_divider_custom_char(void) {
    TEST_ASSERT_EQUAL_STRING("DIV[61]", run("-=").c_str());    // '='
}

// --- literal blocks ---------------------------------------------------------

// FIXES-SUMMARY: `= toggles a literal block.
void test_literal_block_passes_markup_through(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[`!not bold]+lit EOL",
                             runLines({"`=", "`!not bold"}).c_str());
}

void test_literal_block_closes(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[raw]+lit EOL TEXT[x]+b EOL",
                             runLines({"`=", "raw", "`=", "`!x"}).c_str());
}

// --- escapes ----------------------------------------------------------------

void test_escaped_backtick_is_literal(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[a] TEXT[`b] EOL", run("a\\`b").c_str());
}

void test_leading_escape_protects_line_level_markup(void) {
    // "\>x" must be text, not a heading.
    TEST_ASSERT_EQUAL_STRING("TEXT[>x] EOL", run("\\>x").c_str());
}

void test_escaped_divider_is_text(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[-] EOL", run("\\-").c_str());
}

// --- fields -----------------------------------------------------------------

void test_simple_field(void) {
    TEST_ASSERT_EQUAL_STRING("FIELD[user=,w24,text] EOL", run("`<user`>").c_str());
}

void test_field_with_width_and_value(void) {
    TEST_ASSERT_EQUAL_STRING("FIELD[nick=bob,w12,text] EOL", run("`<12|nick`bob>").c_str());
}

void test_checkbox_and_radio(void) {
    TEST_ASSERT_EQUAL_STRING("FIELD[opt=1,w24,check] EOL", run("`<?|opt`1>").c_str());
    TEST_ASSERT_EQUAL_STRING("FIELD[pick=a,w24,radio] EOL", run("`<^|pick`a>").c_str());
}

void test_masked_field(void) {
    TEST_ASSERT_EQUAL_STRING("FIELD[pw=,w8,text,masked] EOL", run("`<!8|pw`>").c_str());
}

// FIXES-SUMMARY: a heading line containing a field loses heading status.
void test_heading_with_field_is_sanitised(void) {
    // The space after the stripped ">" is real content and must survive.
    TEST_ASSERT_EQUAL_STRING("TEXT[ ] FIELD[a=,w24,text] EOL", run("> `<a`>").c_str());
}

// --- anchors ----------------------------------------------------------------

void test_anchor(void) {
    // The anchor name ends at the space; the space itself is content.
    TEST_ASSERT_EQUAL_STRING("ANCHOR[top] TEXT[ Intro] EOL", run("`:top Intro").c_str());
}

// --- alignment --------------------------------------------------------------

void test_alignment(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[mid]+center EOL", run("`cmid").c_str());
}

// --- unimplemented, must degrade quietly ------------------------------------

void test_table_line_is_skipped_not_printed_raw(void) {
    TEST_ASSERT_EQUAL_STRING("", run("`t").c_str());
}

void test_partial_line_is_skipped(void) {
    TEST_ASSERT_EQUAL_STRING("", run("`{something}").c_str());
}

// --- robustness -------------------------------------------------------------

void test_unterminated_link_does_not_run_off_the_end(void) {
    // Malformed markup must not eat content: show the text, drop the marker.
    TEST_ASSERT_EQUAL_STRING("TEXT[a] TEXT[unterminated] EOL", run("a`[unterminated").c_str());
}

void test_unterminated_field_does_not_crash(void) {
    // Same: a page with a typo loses its marker, never its words.
    TEST_ASSERT_EQUAL_STRING("TEXT[a] TEXT[broken] EOL", run("a`<broken").c_str());
}

void test_trailing_backtick(void) {
    TEST_ASSERT_EQUAL_STRING("TEXT[a] EOL", run("a`").c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_text);
    RUN_TEST(test_empty_line_emits_nothing);
    RUN_TEST(test_comment_emits_nothing);
    RUN_TEST(test_bold_toggles);
    RUN_TEST(test_italic_and_underline);
    RUN_TEST(test_backtick_resets_all);
    RUN_TEST(test_lone_backtick_is_reset_not_literal);
    RUN_TEST(test_short_colour_doubles_nibbles);
    RUN_TEST(test_true_colour_form);
    RUN_TEST(test_background_and_reset);
    RUN_TEST(test_malformed_colour_is_dropped_not_printed);
    RUN_TEST(test_link_with_label);
    RUN_TEST(test_link_without_label_uses_target);
    RUN_TEST(test_text_around_link);
    RUN_TEST(test_heading_sets_depth);
    RUN_TEST(test_deeper_heading);
    RUN_TEST(test_depth_persists_then_resets);
    RUN_TEST(test_divider_default_char);
    RUN_TEST(test_divider_custom_char);
    RUN_TEST(test_literal_block_passes_markup_through);
    RUN_TEST(test_literal_block_closes);
    RUN_TEST(test_escaped_backtick_is_literal);
    RUN_TEST(test_leading_escape_protects_line_level_markup);
    RUN_TEST(test_escaped_divider_is_text);
    RUN_TEST(test_simple_field);
    RUN_TEST(test_field_with_width_and_value);
    RUN_TEST(test_checkbox_and_radio);
    RUN_TEST(test_masked_field);
    RUN_TEST(test_heading_with_field_is_sanitised);
    RUN_TEST(test_anchor);
    RUN_TEST(test_alignment);
    RUN_TEST(test_table_line_is_skipped_not_printed_raw);
    RUN_TEST(test_partial_line_is_skipped);
    RUN_TEST(test_unterminated_link_does_not_run_off_the_end);
    RUN_TEST(test_unterminated_field_does_not_crash);
    RUN_TEST(test_trailing_backtick);
    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
