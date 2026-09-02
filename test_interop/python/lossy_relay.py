#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
An in-process UDP relay that can be told to drop traffic.

Both sides of an interop scenario speak UDP to a fixed peer port on loopback,
so putting a forwarder in the middle gives a channel whose loss rate the test
controls, without patching either implementation. That's what "under loss"
requires: the RNS and microReticulum interfaces have no lossy mode of their
own, and simulating loss inside one of them would be testing the simulation.

Wiring (scenario 4):

    C++ RNS  listen 14293  --send--> 14290  [relay]  --send--> 14292  Python RNS
    Python   listen 14292  --send--> 14291  [relay]  --send--> 14293  C++ RNS

Set `cut = True` and nothing crosses in either direction until it's cleared.
"""

import socket
import threading


class LossyRelay:
    def __init__(self, a_listen, a_forward, b_listen, b_forward,
                 host="127.0.0.1", log=print):
        self._pairs = [(a_listen, a_forward), (b_listen, b_forward)]
        self._host = host
        self._log = log
        self._socks = []
        self._threads = []
        self._running = False
        self.cut = False
        self.forwarded = 0
        self.dropped = 0

    def start(self):
        self._running = True
        for listen_port, forward_port in self._pairs:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((self._host, listen_port))
            s.settimeout(0.2)
            self._socks.append(s)
            t = threading.Thread(target=self._pump, args=(s, forward_port),
                                 daemon=True)
            t.start()
            self._threads.append(t)
        self._log(f"[relay] up: {self._pairs[0][0]}->{self._pairs[0][1]}, "
                  f"{self._pairs[1][0]}->{self._pairs[1][1]}")

    def _pump(self, sock, forward_port):
        out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while self._running:
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            if self.cut:
                self.dropped += 1
                continue
            try:
                out.sendto(data, (self._host, forward_port))
                self.forwarded += 1
            except OSError:
                pass

    def stop(self):
        self._running = False
        for s in self._socks:
            try:
                s.close()
            except OSError:
                pass
