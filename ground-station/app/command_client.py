from __future__ import annotations

import argparse
import json
import socket
import time
from pathlib import Path

from .protocol import build_command

DEFAULT_STATIC_HOST = "169.254.10.10"


DANGEROUS_COMMANDS = {
    "FORCE_STOP",
    "HEATERS_OFF",
    "RESET_CTRL",
    "SHUTDOWN_SAFE",
    "OFF",
    "RESET",
    "RADIO_SILENCE",
}


def load_discovered_host(path: Path) -> str | None:
    if not path.exists():
        return None

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError):
        return None

    ip = data.get("onboard_ip")
    if isinstance(ip, str) and ip.strip():
        return ip.strip()
    return None


def discover_onboard_host(discovery_port: int, command_port: int, timeout: float) -> str | None:
    nonce = str(int(time.time() * 1000))
    hello = f"GS_HELLO,{nonce},0,{command_port}\n"

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(timeout)

        sent = False
        for target in ("255.255.255.255", DEFAULT_STATIC_HOST):
            try:
                sock.sendto(hello.encode("utf-8"), (target, discovery_port))
                sent = True
            except OSError:
                continue
        if not sent:
            return None

        end_time = time.time() + timeout
        while time.time() < end_time:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                return None
            except OSError:
                return None

            line = data.decode("utf-8", errors="replace").strip()
            parts = [p.strip() for p in line.split(",")]
            if not parts:
                continue
            # Legacy nonce-matched reply.
            if parts[0] == "ONBOARD_HELLO" and len(parts) >= 6 and parts[1] == nonce:
                return addr[0]
            # New passive broadcast from onboard — no nonce, still authoritative.
            if parts[0] == "ONBOARD_BEACON" and len(parts) >= 5:
                return addr[0]

    return None


def send_command(host: str, port: int, command: str, timeout: float) -> str:
    payload = build_command(command)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload.encode("utf-8"))
        data = sock.recv(4096)
    return data.decode("utf-8", errors="replace").strip()


def add_subparser(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser("command", help="Send one command to onboard")
    parser.add_argument("--host", default=None, help="Onboard host/IP. If omitted, auto-discovery is used.")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--cmd", required=True, help='Example: "STATUS" or "FORCE_START"')
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--yes", action="store_true", help="Skip safety confirmation")
    parser.add_argument("--discovery-enabled", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--discovery-port", type=int, default=4100)
    parser.add_argument("--discover-timeout", type=float, default=2.0)
    parser.add_argument("--discovered-file", type=Path, default=Path("logs/discovered_onboard.json"))
    parser.add_argument("--static-host", default=DEFAULT_STATIC_HOST)
    parser.set_defaults(_coatheal_handler=_handle)


def _handle(args: argparse.Namespace) -> int:
    cmd_name = args.cmd.strip().split()[0].upper()
    if cmd_name in DANGEROUS_COMMANDS and not args.yes:
        confirmation = input(
            f"Command '{cmd_name}' is safety-critical. Type YES to continue: "
        ).strip()
        if confirmation != "YES":
            print("Command cancelled.")
            return 2

    host = args.host
    if host is None:
        host = load_discovered_host(args.discovered_file)
        if host is not None:
            print(f"[command] using discovered onboard host {host}")

    if host is None and args.discovery_enabled:
        host = discover_onboard_host(args.discovery_port, args.port, args.discover_timeout)
        if host is not None:
            print(f"[command] auto-discovered onboard at {host}")

    if host is None:
        host = args.static_host
        print(f"[command] discovery unavailable, using static host {host}")

    response = send_command(host, args.port, args.cmd, args.timeout)
    print(response)
    return 0
