#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Build-time patches to the pinned upstream dependencies.
#
# Why patch at build time instead of vendoring forks: every patch here is a
# portability fix that belongs upstream, and carrying three forks is a
# permanent tax. Patching the fetched tree keeps `platformio.ini`'s pinned SHAs
# as the single source of truth for what we build, and keeps the diff we owe
# upstream visible in one file.
#
# Every patch is anchored to exact upstream text and calls sys.exit(1) if that
# text is not found. A silently-skipped patch is worse than a red build: it
# produces a binary that compiles and misbehaves. If a pin moves, this file is
# where the build tells you.
#
# Patches are idempotent: each is marked with PATCH_MARKER and skipped if the
# marker is already present, because PlatformIO re-runs pre: scripts on every
# invocation against an already-patched tree.

import glob
import os
import sys

Import("env")  # noqa: F821  (injected by SCons/PlatformIO)

MARKER = "patched-by-thicket"
LIBDEPS = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))  # noqa: F821

applied = []
skipped = []


def fail(path, expected, why):
    sys.stderr.write(
        "\n"
        "*** thicket patch_deps.py: FAILED to patch\n"
        "***   file:   %s\n"
        "***   reason: expected upstream text not found\n"
        "***   wanted: %r\n"
        "***   what it is for: %s\n"
        "***\n"
        "*** A pinned dependency moved, or the tree is half-patched. Do not\n"
        "*** work around this by deleting the patch, check the pin in\n"
        "*** platformio.ini against the upstream source and re-anchor.\n\n"
        % (path, expected, why)
    )
    sys.exit(1)


def targets(lib, relpath):
    """Every installed copy of <lib>/<relpath> under this env's libdeps.

    PlatformIO installs a library twice when a dependency is requested both by
    us (as a pinned git URL, giving `MsgPack/`) and by an upstream
    `library.json` (as a name + version range, giving `MsgPack@src-<hash>/`).
    It cannot tell that they are the same package. Only one copy ends up on the
    include path, and it is not reliably ours, so every copy has to be
    patched or the build fails in a way that looks like the patch did not run.
    """
    found = glob.glob(os.path.join(LIBDEPS, lib + "*", relpath))
    if not found:
        fail(os.path.join(LIBDEPS, lib + "*", relpath), "<file exists>",
             "dependency not installed, or its layout changed")
    return sorted(found)


def patch(lib, relpath, patch_id, replacements, why, insert_at_top=None):
    """Apply one named patch to every installed copy of a dependency file.

    `patch_id` makes idempotency per-patch, not per-file: two patches can touch
    the same file (LXStamper.cpp takes both the Arduino-macro prologue and the
    mutex shim) and neither may mask the other.

    Every entry in `replacements` must match at least once, or the build stops.
    A silently-skipped patch is worse than a red build: it produces a binary
    that compiles and misbehaves.
    """
    stamp = "%s:%s" % (MARKER, patch_id)
    for path in targets(lib, relpath):
        rel = os.path.relpath(path, LIBDEPS)

        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()

        if stamp in text:
            skipped.append("%s [%s]" % (rel, patch_id))
            continue

        original = text
        for old, new in replacements:
            if old not in text:
                fail(path, old, why)
            text = text.replace(old, new)

        if insert_at_top:
            text = insert_at_top + text

        if text == original:
            fail(path, "<any change>", why)

        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        applied.append("%s [%s]" % (rel, patch_id))


# ---------------------------------------------------------------------------
# Class A: Arduino.h macro collision.
#
# The Adafruit nRF52 core's Arduino.h defines abs(x) and round(x) as
# function-like macros. Any libstdc++ header pulled in afterwards that names
# std::abs / std::round: <chrono>, reached via <mutex>, via MsgPack, via
# ArduinoJson: fails to parse. The ESP32 Arduino core does not hit this in the
# same include order, which is why upstream never saw it.
#
# Fix: include Arduino.h first, then #undef the two macros for the whole
# translation unit. Include guards make the later Arduino.h includes no-ops,
# and C++ code that wants abs/round gets the <cmath>/<cstdlib> overloads, which
# are correct where the macro was merely convenient.
# ---------------------------------------------------------------------------

ARDUINO_MACRO_PROLOGUE = """// %s:arduino-macros, Arduino.h defines abs()/round()
// as function-like macros. Any libstdc++ header named afterwards that mentions
// std::abs or std::round (<chrono>, reached via <mutex> / MsgPack /
// ArduinoJson) then fails to parse. Pull Arduino.h in first and undefine both
// for this translation unit.
#ifdef ARDUINO
#include <Arduino.h>
#undef abs
#undef round
#endif
""" % MARKER

for _f in (
    "LXMRouter.cpp",
    "LXMessage.cpp",
    "LXStamper.cpp",
    "MessageStore.cpp",
    "PropagationNodeManager.cpp",
):
    patch(
        "microLXMF", os.path.join("src", "LXMF", _f),
        patch_id="arduino-macros",
        replacements=[],
        why="Arduino.h abs/round macros vs libstdc++ <chrono>",
        insert_at_top=ARDUINO_MACRO_PROLOGUE,
    )


# ---------------------------------------------------------------------------
# Class B: no std::mutex in this toolchain.
#
# arm-none-eabi GCC 7.2.1 as shipped by platform nordicnrf52 is built without
# gthreads, so <mutex> declares std::lock_guard (outside the _GLIBCXX_HAS_GTHREADS
# guard, bits/std_mutex.h:156) but NOT std::mutex (inside it, :86).
#
# LXStamper's async stamp slot is guarded by one. The async worker is compiled
# out entirely off ESP32: the xTaskCreatePinnedToCore call sits under
# #ifdef ESP_PLATFORM and the stamper falls back to synchronous: so there is
# no second context to race against and a no-op lock is behaviourally correct.
#
# std::lock_guard is a template over any BasicLockable, so this needs no
# additions to namespace std.
# ---------------------------------------------------------------------------

MUTEX_SHIM = """#include <mutex>

// %s:no-gthreads, this toolchain (arm-none-eabi GCC 7.2.1
// as shipped by platform nordicnrf52) is built without gthreads, so <mutex>
// declares std::lock_guard but not std::mutex. LXStamper's async worker is
// ESP32-only (#ifdef ESP_PLATFORM around xTaskCreatePinnedToCore; the stamper
// is synchronous everywhere else), so nothing races and a no-op lock is
// behaviourally correct. lock_guard templates over any BasicLockable, so this
// needs no additions to namespace std.
#if defined(_GLIBCXX_HAS_GTHREADS)
using ThicketStampMutex = std::mutex;
#else
struct ThicketStampMutex {
	void lock() {}
	void unlock() {}
	bool try_lock() { return true; }
};
#endif
""" % MARKER

patch(
    "microLXMF", os.path.join("src", "LXMF", "LXStamper.cpp"),
    patch_id="no-gthreads",
    replacements=[
        ("#include <mutex>", MUTEX_SHIM),
        ("std::mutex slot_mutex;", "ThicketStampMutex slot_mutex;"),
        ("std::lock_guard<std::mutex>", "std::lock_guard<ThicketStampMutex>"),
    ],
    why="std::mutex absent without gthreads",
)


# ---------------------------------------------------------------------------
# Class C: MsgPack private members.
#
# microLXMF calls arduino::msgpack::Packer::packRawBytes() and reads
# Unpacker::indices: both private in hideakitai/MsgPack v0.4.2, to splice
# pre-encoded msgpack values into the LXMF wire stream. LXMF's field map is
# dict[int, Any] on the wire, so the encoder has to move opaque encoded values
# through without knowing their type.
#
# Upstream microLXMF applies exactly this private:->public: rewrite, but ONLY in
# conformance-bridge/CMakeLists.txt (lines 120-188 at 9876dff). There is no
# PlatformIO equivalent, so every PIO consumer has to reimplement it. The
# anchors below are the same two the CMake patch uses.
# ---------------------------------------------------------------------------

patch(
    "MsgPack", os.path.join("MsgPack", "Packer.h"),
    patch_id="packer-public",
    replacements=[
        (
            "    private:\n        void packRawByte",
            "    public:  // %s:packer-public, was `private:`; microLXMF needs packRawBytes()\n"
            "        void packRawByte" % MARKER,
        ),
    ],
    why="microLXMF splices pre-encoded msgpack values via Packer::packRawBytes",
)

patch(
    "MsgPack", os.path.join("MsgPack", "Unpacker.h"),
    patch_id="unpacker-public",
    replacements=[
        (
            "    class Unpacker {\n        uint8_t* raw_data",
            "    class Unpacker {\n"
            "    public:  // %s:unpacker-public, class-default private; microLXMF reads `indices`\n"
            "        uint8_t* raw_data" % MARKER,
        ),
    ],
    why="microLXMF reads Unpacker::indices to capture key/value byte spans",
)


# ---------------------------------------------------------------------------

print("thicket patch_deps.py: %d applied, %d already current"
      % (len(applied), len(skipped)))
for _p in applied:
    print("  patched  %s" % _p)
