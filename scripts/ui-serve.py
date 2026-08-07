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
SCREENS = os.path.join(ROOT, "screens")

lock = threading.Lock()
proc = None


def start():
    """Bring up the interactive host. Missing binary is not fatal: the
    rendered proposals are still worth serving without it."""
    global proc
    if not os.path.exists(BIN):
        print(f"[ui] no live host at {BIN}")
        print("[ui] build it with: cd tools/uihost && pio run -e native17")
        print("[ui] serving rendered screens only")
        return
    proc = subprocess.Popen([BIN, FRAME], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    proc.stdout.readline()          # "ready"


def send(cmd):
    with lock:
        if proc is None or proc.poll() is not None:
            return False
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        return bool(proc.stdout.readline())


def list_screens():
    if not os.path.isdir(SCREENS):
        return []
    return sorted(f[:-4] for f in os.listdir(SCREENS) if f.endswith(".pbm"))


def pbm_png(path):
    with open(path, "rb") as fh:
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


def frame_png():
    return pbm_png(FRAME)


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
<div class=row id=tabs></div>
<img id=panel alt="virtual panel">
<div class=row id=keys>
  <button data-m=shift>SHIFT</button>
  <button data-m=alt>ALT</button>
  <button data-m=sym>SYM</button>
  <button data-k=enter>ENTER</button>
  <button data-k=back>BACK</button>
</div>
<div id=hint></div>
<script>
const img  = document.getElementById('panel');
const tabs = document.getElementById('tabs');
const keys = document.getElementById('keys');
const hint = document.getElementById('hint');
let mode = 'live';
let timer = null, animFrames = {}, fi = 0;

function refresh(){
  if (timer) { clearInterval(timer); timer = null; }
  if (mode.startsWith('anim:')) {
    const base = mode.slice(5), fr = animFrames[base];
    fi = 0;
    const step = () => { img.src = '/screen/' + fr[fi % fr.length]; fi++; };
    step();
    timer = setInterval(step, 200);
    keys.style.display = 'none';
    hint.textContent = base + ' - ' + fr.length + ' frames looping at 5 fps.';
  } else {
  img.src = (mode === 'live' ? '/frame.png?t=' + Date.now()
                             : '/screen/' + mode + '?t=' + Date.now());
  keys.style.display = (mode === 'live') ? 'flex' : 'none';
  hint.textContent = (mode === 'live')
    ? 'Click the panel, then type. Enter sends, Backspace deletes.'
    : mode + ' - a rendered proposal. Switch to live to type.';
  }
  [...tabs.children].forEach(b =>
    b.style.borderColor = (b.dataset.m2 === mode) ? '#c98b4b' : '#454a3b');
}
function tab(name, label){
  const b = document.createElement('button');
  b.textContent = label; b.dataset.m2 = name;
  b.onclick = () => { mode = name; refresh(); };
  tabs.appendChild(b);
}
async function send(cmd){
  if (mode !== 'live') return;
  await fetch('/key', {method:'POST', body:cmd}); refresh();
}
document.addEventListener('keydown', e => {
  if (e.metaKey || e.ctrlKey || e.altKey || mode !== 'live') return;
  if (e.key === 'Enter')      { e.preventDefault(); send('enter'); }
  else if (e.key === 'Backspace'){ e.preventDefault(); send('back'); }
  else if (e.key.length === 1) { e.preventDefault(); send('k ' + e.key.toLowerCase()); }
});
document.querySelectorAll('[data-m]').forEach(b =>
  b.onclick = () => send('m ' + b.dataset.m));
document.querySelectorAll('[data-k]').forEach(b =>
  b.onclick = () => send(b.dataset.k));

fetch('/screens.json').then(r => r.json()).then(list => {
  tab('live', 'live');
  // a base screen with -f2, -f3 ... siblings is a frame sequence
  list.forEach(n => {
    if (!list.includes(n + '-f2')) return;
    const fr = [n];
    for (let i = 2; list.includes(n + '-f' + i); ++i) fr.push(n + '-f' + i);
    animFrames[n] = fr;
    fr.forEach(f => { const im = new Image(); im.src = '/screen/' + f; });
    tab('anim:' + n, n + ' \u25b6');
  });
  list.forEach(n => tab(n, n));
  refresh();
});
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
            if proc is None:
                self._send(503, b"no live host built", "text/plain")
                return
            try:
                self._send(200, frame_png(), "image/png")
            except Exception as e:
                self._send(500, str(e).encode(), "text/plain")
        elif path == "/screens.json":
            import json as _j
            self._send(200, _j.dumps(list_screens()).encode(),
                       "application/json")
        elif path.startswith("/screen/"):
            name = os.path.basename(path[len("/screen/"):])
            f = os.path.join(SCREENS, name + ".pbm")
            if not os.path.exists(f):
                self._send(404, b"no such screen", "text/plain")
                return
            try:
                self._send(200, pbm_png(f), "image/png")
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
