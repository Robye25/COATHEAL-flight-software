#!/usr/bin/env python3
"""Proves the E-Link hard cap on this machine's kernel (docs/link-budget.md,
"Hard cap").

Builds two throwaway network namespaces joined by a virtual Ethernet cable,
installs the onboard and the ground shaper with scripts/link_cap.sh exactly as
the Pi and the ground station do, then tries to break the cap: both ends flood
UDP of every frame size and push a TCP stream each way, all at once. Every
frame that crosses the cable is captured, and the busiest second is compared
with the cap:

    onboard port   1 800 B      ground port   1 200 B      both   3 000 B (24 kbps)

Nothing outside the namespaces is touched: no real port is capped. Runs as
root, or as an ordinary user where unprivileged user namespaces are allowed.

    python3 scripts/link_cap_selftest.py [--seconds 12] [--json]

Exit 0: the cap held. 1: it did not. 3: this machine cannot run the test.
`--without-cap` leaves the shapers out: the same load must then FAIL, which
shows that the test can.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SCRIPT = Path(__file__).resolve()
LINK_CAP = SCRIPT.parent / "link_cap.sh"
PI_ADDR, GS_ADDR = "10.77.0.1", "10.77.0.2"
PI_IF, GS_IF = "capt-pi0", "capt-gs0"
FLOOD_PORT, STREAM_PORT = 9999, 9998
ONBOARD_SECOND, GROUND_SECOND = 1800, 1200       # bucket + one second of rate
CAP_SECOND = 3000
ETH_P_ALL = 0x0003
PACKET_OUTGOING = 4
CANNOT_RUN = 3


def wire_bytes(frame_len: int) -> int:
    """On-wire bytes of a captured frame: at least 60 B, plus preamble, FCS
    and inter-frame gap (the byte model of docs/link-budget.md)."""
    return max(60, frame_len) + 24


def busiest_second(events: list) -> int:
    """The most bytes in any half-open 1-second window [t, t + 1)."""
    best = total = lo = 0
    for t, n in events:
        total += n
        while events[lo][0] <= t - 1.0:
            total -= events[lo][1]
            lo += 1
        best = max(best, total)
    return best


# ── roles run inside a namespace ─────────────────────────────────────────────
def role_watch(iface: str, seconds: float, out: str) -> int:
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((iface, 0))
    sock.settimeout(0.2)
    sent, received = [], []
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        try:
            data, addr = sock.recvfrom(65535)
        except (socket.timeout, InterruptedError):
            continue
        (sent if addr[2] == PACKET_OUTGOING else received).append(
            (time.monotonic() - start, wire_bytes(len(data))))
    Path(out).write_text(json.dumps({"sent": sent, "received": received}))
    return 0


def role_load(peer: str, seconds: float) -> int:
    """Everything at once: a UDP flood of every frame size, a TCP stream to
    the peer, and a sink for the peer's own flood and stream."""
    stop = time.monotonic() + seconds

    def sink_udp() -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", FLOOD_PORT))
        s.settimeout(0.2)
        while time.monotonic() < stop + 2:
            try:
                s.recv(2048)
            except socket.timeout:
                pass

    def sink_tcp() -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", STREAM_PORT))
        s.listen(1)
        s.settimeout(seconds + 2)
        try:
            conn, _ = s.accept()
        except OSError:
            return
        conn.settimeout(0.5)
        while time.monotonic() < stop + 2:
            try:
                if not conn.recv(65536):
                    break
            except socket.timeout:
                pass
            except OSError:
                break

    def flood_udp() -> None:
        rng = random.Random(os.getpid())
        sizes = [1, 18, 60, 100, 200, 300, 400, 500, 548]   # 548 B fills the 576 B MTU
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setblocking(False)
        while time.monotonic() < stop:
            try:
                s.sendto(b"x" * rng.choice(sizes), (peer, FLOOD_PORT))
            except OSError:
                time.sleep(0.001)

    def stream_tcp() -> None:
        time.sleep(1.0)   # after the peer's listener is up
        try:
            s = socket.create_connection((peer, STREAM_PORT), timeout=seconds)
        except OSError:
            return
        s.settimeout(0.5)
        block = b"y" * 4096
        while time.monotonic() < stop:
            try:
                s.send(block)
            except socket.timeout:
                pass
            except OSError:
                break

    threads = [threading.Thread(target=f, daemon=True) for f in (sink_udp, sink_tcp, flood_udp, stream_tcp)]
    for t in threads:
        t.start()
    while time.monotonic() < stop:
        time.sleep(0.1)
    return 0


# ── the test, inside the outer namespace ─────────────────────────────────────
def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def role_inner(seconds: float, as_json: bool, capped: bool = True) -> int:
    work = Path(tempfile.mkdtemp(prefix="coatheal-linkcap-"))
    ground = subprocess.Popen(["unshare", "-n", "--kill-child", "sleep", "3600"],
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    in_ground = ["nsenter", "-t", str(ground.pid), "-n"]
    me = [sys.executable, str(SCRIPT)]
    try:
        time.sleep(0.3)
        setup = [
            ["ip", "link", "set", "lo", "up"],
            ["ip", "link", "add", PI_IF, "type", "veth", "peer", "name", GS_IF],
            ["ip", "link", "set", GS_IF, "netns", str(ground.pid)],
            ["ip", "addr", "add", f"{PI_ADDR}/24", "dev", PI_IF],
            ["ip", "link", "set", PI_IF, "up"],
            in_ground + ["ip", "link", "set", "lo", "up"],
            in_ground + ["ip", "addr", "add", f"{GS_ADDR}/24", "dev", GS_IF],
            in_ground + ["ip", "link", "set", GS_IF, "up"],
        ]
        for cmd in setup:
            done = run(cmd)
            if done.returncode != 0:
                print(f"[selftest] cannot build the test network: {' '.join(cmd)}: {done.stderr.strip()}")
                return CANNOT_RUN
        for prefix, role, iface in (([], "onboard", PI_IF), (in_ground, "ground", GS_IF)):
            if not capped:
                break
            done = run(prefix + ["bash", str(LINK_CAP), "apply", "--role", role, "--iface", iface,
                                 "--state-file", str(work / f"{role}.state")])
            if done.returncode != 0:
                print(f"[selftest] link_cap.sh could not install the {role} shaper: "
                      f"{(done.stdout + done.stderr).strip()}")
                return CANNOT_RUN
            if not as_json:
                print(done.stdout.strip())
            # `status` is what preflight and the deploy trust: it must agree.
            seen = run(prefix + ["bash", str(LINK_CAP), "status", "--role", role, "--iface", iface,
                                 "--state-file", str(work / f"{role}.state")])
            if seen.returncode != 0:
                print(f"[selftest] FAIL: link_cap.sh status does not see the {role} shaper it installed: "
                      f"{(seen.stdout + seen.stderr).strip()}")
                return 1

        capture = work / "capture.json"
        # Captured on the ground port: what it receives is what the onboard
        # port let onto the cable, what it sends has passed its own shaper.
        watch = subprocess.Popen(in_ground + me + ["--role", "watch", "--iface", GS_IF,
                                                   "--seconds", str(seconds + 5), "--out", str(capture)])
        time.sleep(2.0)   # both buckets full: the worst start
        loads = [subprocess.Popen(me + ["--role", "load", "--peer", GS_ADDR, "--seconds", str(seconds)]),
                 subprocess.Popen(in_ground + me + ["--role", "load", "--peer", PI_ADDR,
                                                    "--seconds", str(seconds)])]
        for proc in loads:
            proc.wait()
        watch.wait()
        events = json.loads(capture.read_text())
    finally:
        # unshare ignores SIGTERM while it waits for its child; killed, it
        # takes the child with it (--kill-child).
        ground.kill()
        ground.wait()
        shutil.rmtree(work, ignore_errors=True)

    from_onboard = [tuple(e) for e in events["received"]]
    from_ground = [tuple(e) for e in events["sent"]]
    both = sorted(from_onboard + from_ground)
    report = {
        "onboard_frames": len(from_onboard),
        "ground_frames": len(from_ground),
        "onboard_worst_second": busiest_second(from_onboard),
        "ground_worst_second": busiest_second(from_ground),
        "total_worst_second": busiest_second(both),
        "largest_frame": max((n for _, n in both), default=0),
        "onboard_limit": ONBOARD_SECOND, "ground_limit": GROUND_SECOND, "total_limit": CAP_SECOND,
    }
    report["total_worst_kbps"] = round(report["total_worst_second"] * 8 / 1000.0, 2)
    # A test that carried nothing proves nothing.
    carried = report["onboard_frames"] >= 20 and report["ground_frames"] >= 20
    held = (report["onboard_worst_second"] <= ONBOARD_SECOND
            and report["ground_worst_second"] <= GROUND_SECOND
            and report["total_worst_second"] <= CAP_SECOND)
    report["result"] = "PASS" if (carried and held) else ("NO TRAFFIC" if not carried else "FAIL")
    if as_json:
        print(json.dumps(report))
    else:
        print(f"[selftest] flooded both ways for {seconds:g} s; busiest second on the cable:")
        print(f"[selftest]   onboard port {report['onboard_worst_second']:5d} B  (limit {ONBOARD_SECOND})"
              f"   {report['onboard_frames']} frames passed")
        print(f"[selftest]   ground port  {report['ground_worst_second']:5d} B  (limit {GROUND_SECOND})"
              f"   {report['ground_frames']} frames passed")
        print(f"[selftest]   both         {report['total_worst_second']:5d} B  (limit {CAP_SECOND})"
              f"   = {report['total_worst_kbps']} kbit/s")
        print(f"[selftest] {report['result']}")
    return 0 if report["result"] == "PASS" else (CANNOT_RUN if not carried else 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--without-cap", action="store_true")
    ap.add_argument("--role", choices=("outer", "inner", "watch", "load"), default="outer")
    ap.add_argument("--iface")
    ap.add_argument("--peer")
    ap.add_argument("--out")
    args = ap.parse_args()

    if args.role == "watch":
        return role_watch(args.iface, args.seconds, args.out)
    if args.role == "load":
        return role_load(args.peer, args.seconds)
    if args.role == "inner":
        return role_inner(args.seconds, args.json, capped=not args.without_cap)

    for tool in ("unshare", "nsenter", "ip", "tc"):
        if shutil.which(tool) is None:
            print(f"[selftest] cannot run: `{tool}` not found")
            return CANNOT_RUN
    isolate = ["unshare", "-n"] if os.geteuid() == 0 else ["unshare", "-Urn"]
    inner = [sys.executable, str(SCRIPT), "--role", "inner", "--seconds", str(args.seconds)]
    if args.json:
        inner.append("--json")
    if args.without_cap:
        inner.append("--without-cap")
    probe = run(isolate + ["true"])
    if probe.returncode != 0:
        print("[selftest] cannot run: no network namespace for this user "
              f"({probe.stderr.strip() or 'unshare failed'}); run it as root")
        return CANNOT_RUN
    return subprocess.call(isolate + ["--kill-child"] + inner)


if __name__ == "__main__":
    sys.exit(main())
