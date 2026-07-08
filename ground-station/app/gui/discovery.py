"""Plug-and-play UDP discovery for the ground station.

Two QThreads:
  * GsBeacon        — broadcasts GS_BEACON every interval seconds.
  * OnboardListener — binds UDP :4100, emits a signal when an ONBOARD_*
                      announcement (legacy HELLO or new BEACON) arrives.
                      Also forwards peer GS_BEACON sightings so the operator
                      can see a conflicting ground station on the network.

Wire format (locked, shared with onboard agent):
    GS_BEACON,<nonce>,<tel_port>,<cmd_port>,<priority>
    ONBOARD_BEACON,<session_id>,<hostname>,<cmd_port>,<tel_port>
    GS_HELLO,<nonce>,<tel_port>,<cmd_port>                          (legacy)
    ONBOARD_HELLO,<nonce>,<session_id>,<hostname>,<cmd_port>,<tel_port>  (legacy)

The two parse helpers at the bottom are pure (no Qt, no sockets) so they can
be unit-tested without a display.
"""
from __future__ import annotations

import socket
import threading
import time
from typing import Optional

from PyQt6.QtCore import QThread, pyqtSignal

try:  # optional enrichment of broadcast addresses
    import psutil  # type: ignore
except Exception:  # pylint: disable=broad-except
    psutil = None  # type: ignore


DISCOVERY_PORT_DEFAULT = 4100
STATIC_ONBOARD_HOST_DEFAULT = "169.254.10.10"


def probe_host_candidates(*hosts: str, include_static: bool = True) -> list[str]:
    """Return ordered, de-duplicated non-empty command-probe hosts."""
    ordered: list[str] = []
    values = list(hosts)
    if include_static:
        values.append(STATIC_ONBOARD_HOST_DEFAULT)
    for host in values:
        if not isinstance(host, str):
            continue
        value = host.strip()
        if value and value not in ordered:
            ordered.append(value)
    return ordered


# ── pure parse helpers (importable without Qt) ───────────────────────────────
def parse_gs_beacon(line: str) -> Optional[dict]:
    """Parse a GS_BEACON line.

    Returns {"nonce": str, "tel_port": int, "cmd_port": int, "priority": int}
    on success, or None on any malformed input.
    """
    if not isinstance(line, str):
        return None
    parts = [p.strip() for p in line.strip().split(",")]
    if len(parts) < 5 or parts[0] != "GS_BEACON":
        return None
    try:
        return {
            "nonce": parts[1],
            "tel_port": int(parts[2]),
            "cmd_port": int(parts[3]),
            "priority": int(parts[4]),
        }
    except (TypeError, ValueError):
        return None


def parse_onboard_announcement(line: str) -> Optional[dict]:
    """Parse either ONBOARD_BEACON or ONBOARD_HELLO into a uniform dict.

    Returns {"kind": "beacon"|"hello", "session_id": str, "hostname": str,
             "cmd_port": int, "tel_port": int, "nonce": str|None} or None.
    """
    if not isinstance(line, str):
        return None
    parts = [p.strip() for p in line.strip().split(",")]
    if not parts:
        return None
    try:
        if parts[0] == "ONBOARD_BEACON" and len(parts) >= 5:
            return {
                "kind": "beacon",
                "session_id": parts[1],
                "hostname": parts[2],
                "cmd_port": int(parts[3]),
                "tel_port": int(parts[4]),
                "nonce": None,
            }
        if parts[0] == "ONBOARD_HELLO" and len(parts) >= 6:
            return {
                "kind": "hello",
                "nonce": parts[1],
                "session_id": parts[2],
                "hostname": parts[3],
                "cmd_port": int(parts[4]),
                "tel_port": int(parts[5]),
            }
    except (TypeError, ValueError):
        return None
    return None


def _enumerate_broadcasts() -> list[str]:
    """Best-effort list of IPv4 broadcast addresses on up interfaces."""
    addrs: list[str] = []
    if psutil is not None:
        try:
            stats = psutil.net_if_stats()
            for name, nics in psutil.net_if_addrs().items():
                if name in stats and not stats[name].isup:
                    continue
                for a in nics:
                    if getattr(a, "family", None) == socket.AF_INET and a.broadcast:
                        if a.broadcast not in addrs:
                            addrs.append(a.broadcast)
        except Exception:  # pylint: disable=broad-except
            pass
    if not addrs:
        try:
            hostname = socket.gethostname()
            _, _, ips = socket.gethostbyname_ex(hostname)
            for ip in ips:
                # naive /24 guess
                parts = ip.split(".")
                if len(parts) == 4:
                    b = ".".join(parts[:3] + ["255"])
                    if b not in addrs:
                        addrs.append(b)
        except Exception:  # pylint: disable=broad-except
            pass
    if not addrs:
        addrs.append("255.255.255.255")
    return addrs


def _discovery_targets() -> list[str]:
    targets = list(_enumerate_broadcasts())
    for host in ("255.255.255.255", STATIC_ONBOARD_HOST_DEFAULT):
        if host not in targets:
            targets.append(host)
    return targets


# ── GS -> broadcast beacon ───────────────────────────────────────────────────
class GsBeacon(QThread):
    """Periodically broadcasts GS_BEACON on every up interface."""

    log_message = pyqtSignal(str)

    def __init__(self, tel_port: int, cmd_port: int,
                 priority: int = 100,
                 discovery_port: int = DISCOVERY_PORT_DEFAULT,
                 interval_s: float = 2.0,
                 parent=None):
        super().__init__(parent)
        self._tel_port = tel_port
        self._cmd_port = cmd_port
        self._priority = max(0, min(999, int(priority)))
        self._disc_port = discovery_port
        self._interval = float(interval_s)
        self._stop = threading.Event()

    def set_priority(self, priority: int) -> None:
        self._priority = max(0, min(999, int(priority)))

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except OSError as exc:
            self.log_message.emit(f"[discovery] beacon socket error: {exc}")
            return
        try:
            last_refresh = 0.0
            targets: list[str] = []
            while not self._stop.is_set():
                now = time.monotonic()
                if now - last_refresh > 30.0:
                    targets = _discovery_targets()
                    last_refresh = now
                nonce = str(int(time.time() * 1000))
                line = (
                    f"GS_BEACON,{nonce},{self._tel_port},"
                    f"{self._cmd_port},{self._priority}\n"
                ).encode("utf-8")
                for addr in targets:
                    try:
                        sock.sendto(line, (addr, self._disc_port))
                    except OSError:
                        continue
                # also legacy GS_HELLO for older onboard builds
                legacy = (
                    f"GS_HELLO,{nonce},{self._tel_port},{self._cmd_port}\n"
                ).encode("utf-8")
                for addr in targets:
                    try:
                        sock.sendto(legacy, (addr, self._disc_port))
                    except OSError:
                        continue
                self._stop.wait(self._interval)
        finally:
            try:
                sock.close()
            except OSError:
                pass


# ── listener for onboard + peer GS announcements ─────────────────────────────
class OnboardListener(QThread):
    """Listens on UDP 0.0.0.0:<discovery_port> for announcements."""

    onboard_discovered = pyqtSignal(str, int, int, str, str)
    # (host, cmd_port, tel_port, session_id, hostname)
    peer_gs_seen = pyqtSignal(str, int)  # (host, priority)
    log_message = pyqtSignal(str)

    _DEDUP_WINDOW_S = 2.0

    def __init__(self, discovery_port: int = DISCOVERY_PORT_DEFAULT, parent=None):
        super().__init__(parent)
        self._port = discovery_port
        self._stop = threading.Event()
        self._last_onboard: tuple = ()
        self._last_onboard_t: float = 0.0
        self._last_peer: tuple = ()
        self._last_peer_t: float = 0.0

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("0.0.0.0", self._port))
            sock.settimeout(0.5)
        except OSError as exc:
            self.log_message.emit(f"[discovery] listener bind failed: {exc}")
            return
        self.log_message.emit(f"[discovery] listening on UDP :{self._port}")
        try:
            while not self._stop.is_set():
                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break
                line = data.decode("utf-8", errors="replace").strip()
                self._handle_line(line, addr[0])
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def _handle_line(self, line: str, src_host: str) -> None:
        onboard = parse_onboard_announcement(line)
        if onboard is not None:
            key = (src_host, onboard["cmd_port"], onboard["tel_port"],
                   onboard["session_id"], onboard["hostname"])
            now = time.monotonic()
            if key == self._last_onboard and (now - self._last_onboard_t) < self._DEDUP_WINDOW_S:
                return
            self._last_onboard = key
            self._last_onboard_t = now
            self.onboard_discovered.emit(src_host, onboard["cmd_port"],
                                         onboard["tel_port"],
                                         onboard["session_id"],
                                         onboard["hostname"])
            return

        peer = parse_gs_beacon(line)
        if peer is not None:
            key = (src_host, peer["priority"])
            now = time.monotonic()
            if key == self._last_peer and (now - self._last_peer_t) < self._DEDUP_WINDOW_S:
                return
            self._last_peer = key
            self._last_peer_t = now
            self.peer_gs_seen.emit(src_host, peer["priority"])


class CommandProbe(QThread):
    """Low-rate TCP command probe for deterministic link-local bring-up."""

    onboard_reachable = pyqtSignal(str, int)
    log_message = pyqtSignal(str)

    def __init__(self, hosts: list[str], cmd_port: int,
                 interval_s: float = 2.0,
                 timeout_s: float = 0.7,
                 include_static: bool = True,
                 parent=None):
        super().__init__(parent)
        self._include_static = include_static
        self._hosts = probe_host_candidates(*hosts, include_static=include_static)
        self._cmd_port = int(cmd_port)
        self._interval = float(interval_s)
        self._timeout = float(timeout_s)
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._last_success: tuple[str, int] | None = None

    def set_candidates(self, hosts: list[str]) -> None:
        with self._lock:
            self._hosts = probe_host_candidates(*hosts,
                                                include_static=self._include_static)

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        while not self._stop.is_set():
            with self._lock:
                hosts = list(self._hosts)
            for host in hosts:
                if self._stop.is_set():
                    break
                if self._try_ping(host):
                    key = (host, self._cmd_port)
                    if key != self._last_success:
                        self.log_message.emit(f"[discovery] command probe {host}:{self._cmd_port}")
                        self._last_success = key
                    self.onboard_reachable.emit(host, self._cmd_port)
                    break
            self._stop.wait(self._interval)

    def _try_ping(self, host: str) -> bool:
        try:
            with socket.create_connection((host, self._cmd_port), timeout=self._timeout) as sock:
                sock.settimeout(self._timeout)
                sock.sendall(b"PING\n")
                data = sock.recv(256)
        except OSError:
            return False
        return data.decode("utf-8", errors="replace").strip().startswith("ACK,PING")
