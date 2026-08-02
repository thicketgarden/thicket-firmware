// Copyright (C) 2026 Thicket contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ---------------------------------------------------------------------------
// Micron — a streaming parser for NomadNet's page markup.
//
// WHY THIS SHAPE
//
// This is a SAX-style parser: you feed it a line, it calls back with styled
// spans. It builds no document tree and allocates nothing per element.
//
// That is not a stylistic preference, it is the constraint. tasks/T13 budgets
// roughly 113-146 KB of SRAM for everything M2 adds. The nearest comparable
// renderer (reticulous/nomad's Micron->LVGL) caps itself at 600 retained LVGL
// objects and still requires 8 MB of PSRAM, because it holds an object tree
// per page. We cannot, so we do not: parse and draw straight into the
// framebuffer, keep the source bytes and a scroll offset, re-render on change.
// A Sharp memory LCD holds its own image with the CPU asleep, so the panel IS
// the retained model and re-rendering is rare.
//
// GRAMMAR OF RECORD
//
// markqvist/NomadNet, nomadnet/ui/textui/MicronParser.py, read 2026-08-01.
// Implemented against that file directly, NOT against a summary --- an earlier
// second-hand summary in docs/nomadnet-services.md got four things wrong:
// it had `= as a divider (it is the literal toggle), a lone backtick as the
// literal toggle (it is a style reset), no tables, and no `FT/`BT true-colour
// forms. See MICRON.md for the full delta.
//
// NOT YET IMPLEMENTED, deliberately, each a no-op that does not corrupt the
// rest of the line:  `t tables  ·  `{ partials
// ---------------------------------------------------------------------------

#ifndef THICKET_MICRON_H
#define THICKET_MICRON_H

#include <stddef.h>
#include <stdint.h>

namespace micron {

enum class Align : uint8_t { Left, Center, Right };

// A colour is 24-bit RGB, or "the renderer's default".
// Micron carries explicit colour; a 1-bit panel has two inks. Resolving that is
// the RENDERER's job, not the parser's --- the parser reports what the page
// asked for and never decides what it means. See MICRON.md.
struct Color {
    uint32_t rgb = 0;          // 0xRRGGBB
    bool     is_default = true;

    bool operator==(const Color& o) const {
        return is_default == o.is_default && (is_default || rgb == o.rgb);
    }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

struct Style {
    bool  bold      = false;
    bool  italic    = false;
    bool  underline = false;
    Color fg;
    Color bg;
    Align align     = Align::Left;
    uint8_t depth   = 0;       // section depth; indent is depth * SECTION_INDENT
    bool  literal   = false;   // inside a `= block: emit verbatim, no markup

    bool operator==(const Style& o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline
            && fg == o.fg && bg == o.bg && align == o.align
            && depth == o.depth && literal == o.literal;
    }
};

// NomadNet indents section content by 2 columns per depth level.
static constexpr uint8_t SECTION_INDENT = 2;

// MicronParser.py's default when a field declares no width.
static constexpr uint8_t DEFAULT_FIELD_WIDTH = 24;

enum class FieldKind : uint8_t { Text, Checkbox, Radio };

struct Field {
    const char* name     = nullptr;  // NOT null-terminated; use name_len
    size_t      name_len = 0;
    const char* value    = nullptr;  // preset value / label text
    size_t      value_len = 0;
    uint8_t     width    = DEFAULT_FIELD_WIDTH;
    FieldKind   kind     = FieldKind::Text;
    bool        masked   = false;    // render as asterisks
    bool        prechecked = false;
};

// Renderer interface. Implement this for text, for the Sharp panel, for tests.
//
// Every pointer handed to a callback points INTO THE CALLER'S LINE BUFFER and
// is invalid once the callback returns. Copy what you need. This is what keeps
// the parser allocation-free.
class Renderer {
public:
    virtual ~Renderer() = default;

    // A run of text in one style. Never spans a line.
    virtual void onText(const char* text, size_t len, const Style& style) = 0;

    // `[label`target] --- label may be empty, in which case target is the label.
    virtual void onLink(const char* label, size_t label_len,
                        const char* target, size_t target_len,
                        const Style& style) = 0;

    // A line starting with '-'. `ch` is the fill character (UTF-8 codepoint).
    virtual void onDivider(uint32_t ch, const Style& style) = 0;

    // `<...`...> form widget.
    virtual void onField(const Field& field, const Style& style) = 0;

    // `:name --- a zero-width named position, for in-document links.
    virtual void onAnchor(const char* name, size_t len) = 0;

    // Called once per input line that produced any output, after its content.
    // Not called for comments, literal toggles, or empty results.
    virtual void onLineEnd(const Style& style) = 0;
};

class Parser {
public:
    // Feed one line, without its terminator. Safe to call with len == 0.
    void parseLine(const char* line, size_t len, Renderer& out);

    // Reset to document-start state. Call between pages --- style, section
    // depth and literal mode all persist across lines by design.
    void reset() { _style = Style{}; }

    const Style& style() const { return _style; }

private:
    Style _style;

    // Inline markup pass. `pre_escape` means the line began with a backslash,
    // so its first character is literal.
    void emitInline(const char* line, size_t len, Renderer& out, bool pre_escape);
};

} // namespace micron

#endif // THICKET_MICRON_H
