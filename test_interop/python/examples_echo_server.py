#!/usr/bin/env python3
# Copyright (C) 2026 Thicket contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Runs the reference implementation's own Examples/Echo.py as the peer.
#
# Echo.py is NOT modified & NOT reimplemented. This wrapper only does the two
# things the script can't do for itself in an unattended run: give it a config
# with a UDP interface that reaches our C++ side, & press enter for it, since
# its server mode announces on input() rather than on a timer.
#
# The point of testing against upstream's example instead of a script of our
# own: a script we wrote encodes our reading of the protocol. If we misread it,
# our script misreads it the same way & the test passes anyway.

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RNS_REPO = "https://github.com/markqvist/Reticulum.git"


def rns_version():
    import RNS
    return RNS.__version__


def fetch_examples(dest):
    """Clone the reference at the version we have installed, for its Examples."""
    version = rns_version()
    print(f"[echo-server] fetching Examples/ from the reference at {version}",
          flush=True)
    subprocess.run(
        ["git", "clone", "--depth", "1", "--branch", version, "-q", RNS_REPO, dest],
        check=True)
    script = os.path.join(dest, "Examples", "Echo.py")
    if not os.path.isfile(script):
        sys.exit(f"[echo-server] no Examples/Echo.py at {version}")
    return script


def write_config(configdir, local_port, remote_port):
    os.makedirs(configdir, exist_ok=True)
    with open(os.path.join(configdir, "config"), "w") as f:
        # `interface_enabled`, not `enabled`. RNS ignores the latter and the
        # interface stays down, which presents as a peer that announces
        # cheerfully into nothing.
        f.write(f"""
[reticulum]
  enable_transport = No
  share_instance = No
  shared_instance_port = 37448
  instance_control_port = 37449
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  [[UDPInterop]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = {local_port}
    forward_ip = 127.0.0.1
    forward_port = {remote_port}
""")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float,
                    default=float(os.environ.get("THICKET_INTEROP_TIMEOUT_S", "90")))
    args, _ = ap.parse_known_args()
    timeout = args.timeout
    workdir = tempfile.mkdtemp(prefix="thicket-examples-echo-")
    try:
        script = fetch_examples(os.path.join(workdir, "reticulum"))
        configdir = os.path.join(workdir, "config")
        # Our C++ side listens on 14287 and forwards to 14286.
        write_config(configdir, local_port=14286, remote_port=14287)

        print("[echo-server] starting Examples/Echo.py -s, unmodified", flush=True)
        proc = subprocess.Popen(
            [sys.executable, script, "-s", "--config", configdir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)

        # The server half asserts something of its own rather than reporting
        # success for having started. Echo.py logs this line only after it has
        # decrypted a packet addressed to it and returned a proof, so seeing
        # it's evidence the reference accepted our crypto.
        seen = {"echo_request": False}

        def pump():
            for line in proc.stdout:
                print("[Echo.py] " + line.rstrip(), flush=True)
                if "Received packet from echo client" in line:
                    seen["echo_request"] = True
        threading.Thread(target=pump, daemon=True).start()

        # Echo.py announces when the user presses enter. Do that on a timer so
        # the client has something to hear, without touching the script.
        deadline = time.time() + timeout
        while time.time() < deadline and proc.poll() is None:
            try:
                proc.stdin.write("\n")
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                break
            time.sleep(5)

        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

        if seen["echo_request"]:
            print("[echo-server] SUCCESS: the reference decrypted a packet from "
                  "us and returned a proof", flush=True)
            return 0
        print("[echo-server] FAIL: Echo.py never reported receiving a packet "
              "from the client", flush=True)
        return 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
