#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Interactive virtual panel in a browser.
#
# Owns a tools/uihost process, pipes key events to it, and serves the frame it
# renders. The pixels come from the real SharpLcd driver through VirtualPanel,
# so what is on screen is what the panel would show.
#
#   python3 scripts/ui-serve.py          http://127.0.0.1:8770
#   python3 scripts/ui-serve.py --lan

import argparse
import os
import subprocess
import sys
import threading
import zlib
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tools", "uihost", ".pio", "build", "native17", "program")
FRAME = "/tmp/thicket_ui_frame.pbm"

lock = threading.Lock()
proc = None


def start():
    global proc
    if not os.path.exists(BIN):
        sys.exit(f"[ui] no binary at {BIN}\n"
                 f"[ui] build it: cd tools/uihost && pio run -e native17")
    proc = subprocess.Popen([BIN, FRAME], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    proc.stdout.readline()          # "ready"


def send(cmd):
    with lock:
        if proc.poll() is not None:
            return False
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        return bool(proc.stdout.readline())


def frame_png():
    with open(FRAME, "rb") as fh:
        tok = [l.split(b"#", 1)[0].strip() for l in fh]
    tok = [t for t in tok if t]
    w, h = (int(v) for v in tok[1].split())
    bits = bytes(c for c in b"".join(tok[2:]) if c in (48, 49))

    rows = bytearray()
    for y in range(h):
        rows.append(0)
        rows.extend(0 if c == 49 else 255 for c in bits[y * w:(y + 1) * w])

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


PAGE = """<!doctype html><meta charset=utf8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Thicket panel</title>
<style>
 body{margin:0;background:#1b1d18;color:#cfcabb;font:13px ui-monospace,Menlo,monospace;
      display:flex;flex-direction:column;align-items:center;gap:.9rem;padding:1.4rem}
 #panel{image-rendering:pixelated;width:min(90vw,800px);border:10px solid #33372c;
        border-radius:6px;background:#fff}
 kbd{background:#2c3026;border:1px solid #454a3b;border-radius:4px;padding:1px 5px}
 .row{display:flex;gap:1rem;flex-wrap:wrap;justify-content:center}
 button{font:inherit;background:#2c3026;color:#cfcabb;border:1px solid #454a3b;
        border-radius:5px;padding:.3rem .7rem;cursor:pointer}
 button:hover{background:#3a3f31}
 #hint{color:#8b8878}
</style>
<img id=panel alt="virtual panel">
<div class=row>
  <button data-m=shift>SHIFT</button>
  <button data-m=alt>ALT</button>
  <button data-m=sym>SYM</button>
  <button data-k=enter>ENTER</button>
  <button data-k=back>BACK</button>
</div>
<div id=hint>Click the panel, then type. <kbd>Enter</kbd> sends, <kbd>Backspace</kbd> deletes.</div>
<script>
const img = document.getElementById('panel');
function refresh(){ img.src = '/frame.png?t=' + Date.now(); }
async function send(cmd){ await fetch('/key', {method:'POST', body:cmd}); refresh(); }
document.addEventListener('keydown', e => {
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if (e.key === 'Enter')      { e.preventDefault(); send('enter'); }
  else if (e.key === 'Backspace'){ e.preventDefault(); send('back'); }
  else if (e.key.length === 1) { e.preventDefault(); send('k ' + e.key.toLowerCase()); }
});
document.querySelectorAll('[data-m]').forEach(b =>
  b.onclick = () => send('m ' + b.dataset.m));
document.querySelectorAll('[data-k]').forEach(b =>
  b.onclick = () => send(b.dataset.k));
refresh();
</script>
"""


class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode(), "text/html; charset=utf-8")
        elif path == "/frame.png":
            try:
                self._send(200, frame_png(), "image/png")
            except Exception as e:
                self._send(500, str(e).encode(), "text/plain")
        else:
            self._send(404, b"not here", "text/plain")

    def do_POST(self):
        if self.path != "/key":
            self._send(404, b"not here", "text/plain")
            return
        n = int(self.headers.get("Content-Length") or 0)
        cmd = self.rfile.read(n).decode(errors="replace").strip("\r\n")
        # Only newlines: a plain strip() eats the trailing space in "k ",
        # so the space bar silently does nothing.
        ok = send(cmd)
        self._send(200 if ok else 500, b"ok" if ok else b"uihost died",
                   "text/plain")

    def log_message(self, *_):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--lan", action="store_true")
    args = ap.parse_args()

    start()
    host = "0.0.0.0" if args.lan else "127.0.0.1"
    srv = ThreadingHTTPServer((host, args.port), H)
    print(f"[ui] panel at http://127.0.0.1:{args.port}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[ui] stopped")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()


if __name__ == "__main__":
    main()
