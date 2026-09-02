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
#      A failed one warns that the target is not in DFU mode and still exits
#      zero. `flash` keys on "Device programmed" rather than the exit code.
#      Do NOT use elapsed time as the failure signal: this times the whole
#      `pio run -t upload`, so a fresh env compiling from scratch legitimately
#      takes minutes.
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


def port_serials():
    """Map each usbmodem port to its USB serial number.

    Port names are NOT stable identifiers. Flashing re-enumerates the device
    and macOS reassigns the name, so two boards can swap between one command
    and the next. The serial number does not move.
    """
    out = {}
    try:
        ioreg = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l"],
                               capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return out
    for port in sorted(glob.glob("/dev/cu.usbmodem*")):
        name = os.path.basename(port)
        idx = ioreg.find(name)
        if idx < 0:
            continue
        head = ioreg[:idx]
        marker = head.rfind('"USB Serial Number" = "')
        if marker < 0:
            continue
        serial = head[marker + len('"USB Serial Number" = "'):]
        serial = serial.split('"', 1)[0]
        out[port] = serial
    return out


def find_port(explicit=None, serial=None):
    """Resolve a port, refusing to guess when more than one board is attached.

    Picking the first of several is how the wrong board gets flashed, and the
    mistake is invisible until something downstream reads wrong.
    """
    if explicit:
        return explicit

    serial = serial or os.environ.get("THICKET_BOARD_SERIAL")
    mapping = port_serials()

    if serial:
        matches = [p for p, s in mapping.items() if s.lower() == serial.lower()]
        if len(matches) == 1:
            return matches[0]
        if not matches:
            known = "".join(f"\n    {p}  {s}" for p, s in sorted(mapping.items()))
            sys.exit(f"no board with serial {serial}. Attached:{known or ' none'}")
        sys.exit(f"serial {serial} matches several ports: {matches}")

    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(
        glob.glob("/dev/tty.usbmodem*"))
    if not ports:
        return None
    if len(ports) == 1:
        return ports[0]

    listing = "".join(f"\n    {p}  {mapping.get(p, '?')}" for p in ports)
    sys.exit(
        f"{len(ports)} boards attached, refusing to guess which one.{listing}\n"
        f"Pass --serial <SERIAL>, --port <PORT>, or set THICKET_BOARD_SERIAL. "
        f"Use `board.py boards` to list them.")


def cmd_boards(_args):
    mapping = port_serials()
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        print("[board] no usbmodem ports")
        return
    for p in ports:
        print(f"  {p}  serial={mapping.get(p, '?')}")


class Capture(threading.Thread):
    """Opens the port the moment it exists and reads until told to stop.

    Runs as a thread so it can be started BEFORE the reset that produces the
    output being captured. Reconnects across the enumeration gap, because the
    port disappears while the board resets and comes back a moment later.
    """

    def __init__(self, port, baud=DEFAULT_BAUD, echo=False, poke=False):
        """echo=True streams each line as it arrives; echo=False keeps the
        collect-then-return behaviour that `flash` relies on for its summary.

        poke=True writes a newline once the port is open. The firmware restates
        its addresses on any serial input, which is the only way to read them
        after a power cycle: USB CDC discards writes made while no host is
        listening, so the boot banner is already gone by the time we attach."""
        super().__init__(daemon=True)
        self.echo = echo
        self.poke = poke
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
                    if self.poke:
                        time.sleep(0.3)   # let the CDC endpoint settle
                        try:
                            ser.write(b"\n")
                            ser.flush()
                        except Exception:
                            pass
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
                    text = line.decode("utf-8", "replace").rstrip()
                    self.lines.append(text)
                    # Emit immediately rather than buffering until the timer
                    # expires. A long capture that prints nothing until it ends
                    # is indistinguishable from a dead board, and costs a real
                    # test on 2026-08-05: a message arrived, the log looked
                    # empty, the capture was restarted to "fix" it, and the
                    # exchange went with it. Watching a board is the whole job
                    # of this command.
                    if self.echo:
                        print(text, flush=True)
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
    port = find_port(args.port, getattr(args, 'serial', None))
    if not port:
        sys.exit("no usbmodem port found")
    cap = Capture(port, echo=True, poke=getattr(args, "poke", False))
    cap.start()
    print(f"[board] capturing {port} for {args.seconds}s"
          f"{' (poking for a restatement)' if getattr(args, 'poke', False) else ''}", flush=True)
    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        print("[board] interrupted", flush=True)
    cap.stop()
    print(f"[board] {len(cap.lines)} lines")


def cmd_flash(args):
    port = find_port(args.port, getattr(args, 'serial', None))
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
        # --upload-port is not optional here. Without it PlatformIO does its
        # own port detection, which with two boards attached picks one of them
        # arbitrarily -- so the upload can land on a different board than the
        # one this command resolved and is about to read back from. The verify
        # then passes against a board nobody chose.
        ["pio", "run", "-e", args.env, "-t", "upload", "--upload-port", port],
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

    if verdict_bad:
        cap.stop()
        print("--- upload output ---")
        print(out[-3000:])
        sys.exit(f"[board] FLASH DID NOT LAND: {verdict_bad}")

    print(f"[board] flash verified (took {elapsed:.0f}s, 'Device programmed')",
          flush=True)
    if elapsed > 120:
        # Not a failure. This times the whole `pio run -t upload`, so a fresh
        # env that has to compile from scratch legitimately takes minutes.
        # "Device programmed" above is the real signal.
        print(f"[board] (slow: {elapsed:.0f}s -- almost certainly a full "
              f"rebuild, not an upload problem)", flush=True)
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
    ap.add_argument("--serial", default=None,
                    help="select the board by USB serial number; stable "
                         "across re-enumeration where the port name is not")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("boards")
    p.set_defaults(func=cmd_boards)

    p = sub.add_parser("ports")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("capture")
    p.add_argument("--seconds", type=float, default=15)
    p.add_argument("--poke", action="store_true",
                   help="send a newline once open; the firmware restates its "
                        "addresses, which is the only way to read them after a "
                        "power cycle")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("flash")
    p.add_argument("env")
    p.add_argument("--seconds", type=float, default=20)
    p.set_defaults(func=cmd_flash)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
