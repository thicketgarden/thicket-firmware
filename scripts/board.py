#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Drive the RAK4631 without a human at the bench.
#
# Three things about this board make unattended work fail in ways that look
# like something else, and all three are handled here rather than remembered:
#
#   1. A PlatformIO upload can print SUCCESS without programming anything.
#      A real upload takes roughly 35-45 s and prints "Device programmed".
#      A failed one takes about six minutes, warns that the target is not in
#      DFU mode, and still exits zero. `flash` times the upload and calls that
#      out instead of trusting the exit code.
#
#   2. The boot banner is gone before you can read it. USB-CDC buffers nothing
#      before a host opens the port, and the firmware prints once at startup
#      and then idles. Opening the port a second after reset captures nothing,
#      which reads exactly like a dead board. `capture` polls for the port
#      every 20 ms and opens it the instant it enumerates.
#
#   3. The 1200-baud touch does not reset this board, whatever the Adafruit
#      core documents. adafruit-nrfutil resets it fine on its own, so `flash`
#      is the only reliable way to get a board from one state to another --
#      there is no "just reset it" here.
#
# Usage:
#   python3 scripts/board.py capture [--seconds N] [--port P]
#   python3 scripts/board.py flash <env> [--seconds N]
#   python3 scripts/board.py ports

import argparse
import glob
import os
import subprocess
import sys
import threading
import time

DEFAULT_BAUD = 115200


def find_port(explicit=None):
    if explicit:
        return explicit
    for pat in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


class Capture(threading.Thread):
    """Opens the port the moment it exists and reads until told to stop.

    Runs as a thread so it can be started BEFORE the reset that produces the
    output being captured. Reconnects across the enumeration gap, because the
    port disappears while the board resets and comes back a moment later.
    """

    def __init__(self, port, baud=DEFAULT_BAUD):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.lines = []
        self._stopev = threading.Event()
        self.connected_at = None

    def run(self):
        import serial
        ser = None
        buf = b""
        while not self._stopev.is_set():
            if ser is None:
                if not os.path.exists(self.port):
                    time.sleep(0.02)
                    continue
                try:
                    ser = serial.Serial(self.port, self.baud, timeout=0.05)
                    if self.connected_at is None:
                        self.connected_at = time.time()
                except Exception:
                    ser = None
                    time.sleep(0.02)
                    continue
            try:
                chunk = ser.read(256)
            except Exception:
                # The port vanished mid-read: the board reset under us.
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                continue
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.lines.append(line.decode("utf-8", "replace").rstrip())
        if ser:
            try:
                ser.close()
            except Exception:
                pass

    def stop(self):
        self._stopev.set()
        self.join(timeout=3)


def cmd_ports(_args):
    for pat in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*"):
        for p in sorted(glob.glob(pat)):
            print(p)
    vols = [v for v in os.listdir("/Volumes")]
    print("volumes:", ", ".join(vols))


def cmd_capture(args):
    port = find_port(args.port)
    if not port:
        sys.exit("no usbmodem port found")
    cap = Capture(port)
    cap.start()
    print(f"[board] capturing {port} for {args.seconds}s", flush=True)
    time.sleep(args.seconds)
    cap.stop()
    for line in cap.lines:
        print(line)
    print(f"[board] {len(cap.lines)} lines")


def cmd_flash(args):
    port = find_port(args.port)
    if not port:
        sys.exit("no usbmodem port found")

    # Do NOT hold the port open across the upload. nrfutil drives DFU over this
    # same port, and a second reader makes it fail with "device reports
    # readiness to read but returned no data (multiple access on port?)" --
    # which looks like a bootloader mismatch and is not. Capture starts the
    # moment nrfutil exits and polls every 20 ms, which is fast enough to catch
    # the banner the board prints as it comes back up.
    print(f"[board] uploading {args.env}", flush=True)
    started = time.time()
    proc = subprocess.run(
        ["pio", "run", "-e", args.env, "-t", "upload"],
        capture_output=True, text=True)
    elapsed = time.time() - started
    # Attach before doing anything else with the result.
    cap = Capture(port)
    cap.start()
    out = proc.stdout + proc.stderr

    programmed = "Device programmed" in out
    not_in_dfu = "not in DFU mode" in out or "Target is not in DFU" in out

    print(f"[board] upload finished in {elapsed:.0f}s, exit {proc.returncode}",
          flush=True)

    verdict_bad = None
    if proc.returncode != 0:
        verdict_bad = f"pio exited {proc.returncode}"
    elif not_in_dfu:
        verdict_bad = "nrfutil reported the target was not in DFU mode"
    elif not programmed:
        verdict_bad = "no 'Device programmed' in the upload output"
    elif elapsed > 120:
        verdict_bad = (f"upload took {elapsed:.0f}s; a real one is 35-45s, so "
                       f"this almost certainly did not program")

    if verdict_bad:
        cap.stop()
        print("--- upload output ---")
        print(out[-3000:])
        sys.exit(f"[board] FLASH DID NOT LAND: {verdict_bad}")

    print(f"[board] flash verified (took {elapsed:.0f}s, 'Device programmed')",
          flush=True)
    print(f"[board] listening {args.seconds}s for boot output", flush=True)
    time.sleep(args.seconds)
    cap.stop()
    print("--- board output ---")
    for line in cap.lines:
        print(line)
    print(f"[board] {len(cap.lines)} lines captured")
    if not cap.lines:
        print("[board] NOTE: no output. The flash is verified above, so this "
              "means the firmware is silent or faulted before its first "
              "print -- not that the upload failed.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ports")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("capture")
    p.add_argument("--seconds", type=float, default=15)
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("flash")
    p.add_argument("env")
    p.add_argument("--seconds", type=float, default=20)
    p.set_defaults(func=cmd_flash)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
