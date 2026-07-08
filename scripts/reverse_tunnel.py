"""Reverse port-forward Pi:4000 -> laptop:4000 via the SSH session.

Keeps running until killed (Ctrl-C / process kill). Pi's onboard (with
comms.telemetry_host=127.0.0.1) pushes frames to its own loopback; SSH
tunnels each incoming connection back to our laptop's localhost:4000 where
the ground-station listens.
"""
from __future__ import annotations

import getpass
import os
import select
import socket
import threading

import paramiko

HOST = os.environ.get("COATHEAL_PI_HOST", "169.254.10.10")
USER = os.environ.get("COATHEAL_PI_USER", "coatheal")
PASSWORD = os.environ.get("COATHEAL_PI_PASSWORD")
REMOTE_PORT = 4000
LOCAL_HOST = "127.0.0.1"
LOCAL_PORT = 4000


def _handle(chan, sock):
    while True:
        r, _, _ = select.select([sock, chan], [], [])
        if sock in r:
            data = sock.recv(4096)
            if not data:
                break
            chan.send(data)
        if chan in r:
            data = chan.recv(4096)
            if not data:
                break
            sock.send(data)
    try: chan.close()
    except Exception: pass
    try: sock.close()
    except Exception: pass


def main() -> None:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    password = PASSWORD or getpass.getpass("Pi password: ")
    c.connect(HOST, username=USER, password=password, timeout=15)
    t = c.get_transport()
    assert t is not None
    t.request_port_forward("127.0.0.1", REMOTE_PORT)
    print(f"[tunnel] Pi 127.0.0.1:{REMOTE_PORT} -> laptop {LOCAL_HOST}:{LOCAL_PORT}")
    try:
        while True:
            chan = t.accept(1000)
            if chan is None:
                continue
            try:
                sock = socket.create_connection((LOCAL_HOST, LOCAL_PORT), timeout=5)
            except OSError as exc:
                print(f"[tunnel] local connect failed: {exc}")
                chan.close(); continue
            threading.Thread(target=_handle, args=(chan, sock), daemon=True).start()
    finally:
        c.close()


if __name__ == "__main__":
    main()
