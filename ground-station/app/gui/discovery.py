"""Plug-and-play UDP discovery for the ground station.

Three QThreads:
  * GsBeacon        — broadcasts GS_BEACON (and the legacy GS_HELLO) every
                      2 s while no telemetry arrives, GS_BEACON alone every
                      15 s while it does (docs/link-budget.md).
  * OnboardListener — binds UDP :4100, emits a signal when an ONBOARD_*
                      announcement (legacy HELLO or new BEACON) arrives.
                      Also forwards peer GS_BEACON sightings so the operator
                      can see a conflicting ground station on the network.
  * CommandProbe    — PINGs the candidate command hosts every 2 s while no
                      telemetry arrives.

The beacon and the probe charge the ground station's link budget
(`link_budget.ground_budget()`) and never wait for it.

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

from ..link_budget import (
    DISCOVERY_HEALTHY_INTERVAL_S, DISCOVERY_INTERVAL_S, DiscoveryRounds, LinkBudget, Priority,
    command_exchange_bytes, ground_budget, paced_connection,
)

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


class SentNonceRegistry:
    """Thread-safe bounded record of beacon nonces this process has sent.

    Broadcasts loop back to every listener on the sending machine, so the
    OnboardListener receives our own GS_BEACON every cycle. Recognising our
    own nonces is what lets it drop those instead of reporting this ground
    station as a conflicting peer of itself. Bounded so a long-running GUI
    never grows it: beacons go out every ~2 s and the loopback copy arrives
    within milliseconds, so remembering the last few is already generous.
    """

    def __init__(self, capacity: int = 64):
        self._capacity = max(1, int(capacity))
        self._order: list[str] = []
        self._known: set[str] = set()
        self._lock = threading.Lock()

    def add(self, nonce: str) -> None:
        with self._lock:
            if nonce in self._known:
                return
            self._order.append(nonce)
            self._known.add(nonce)
            while len(self._order) > self._capacity:
                self._known.discard(self._order.pop(0))

    def was_sent(self, nonce: str) -> bool:
        with self._lock:
            return nonce in self._known


class PeerSightingThrottle:
    """Decides when a peer-GS sighting is worth reporting again.

    A healthy peer beacons every ~2 s; reporting each one floods the event
    log. Report a (host, priority) pair when it is first seen and then at
    most once per `reseen_s`; a *different* host or a priority change is
    news and reports immediately.
    """

    def __init__(self, reseen_s: float = 300.0):
        self._reseen_s = float(reseen_s)
        self._last_emit: dict[tuple[str, int], float] = {}

    def should_emit(self, host: str, priority: int, now: float) -> bool:
        key = (host, int(priority))
        last = self._last_emit.get(key)
        if last is not None and (now - last) < self._reseen_s:
            return False
        self._last_emit[key] = now
        return True


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
    """Broadcasts GS_BEACON on every up interface: every 2 s together with
    the legacy GS_HELLO while no telemetry arrives, GS_BEACON alone every
    15 s while it does (docs/link-budget.md). Every datagram is charged to
    the ground station's link budget; one the budget cannot take is skipped
    and tried again at the next check of the same round -- the beacon thread
    never waits for the budget."""

    log_message = pyqtSignal(str)

    # The longest sleep between checks: a link that drops mid-way through a
    # 15 s wait brings the 2 s cadence back within this.
    _CHECK_S = 0.5
    _TARGET_REFRESH_S = 30.0

    def __init__(self, tel_port: int, cmd_port: int,
                 priority: int = 100,
                 discovery_port: int = DISCOVERY_PORT_DEFAULT,
                 interval_s: float = DISCOVERY_INTERVAL_S,
                 sent_nonces: Optional[SentNonceRegistry] = None,
                 healthy_interval_s: float = DISCOVERY_HEALTHY_INTERVAL_S,
                 budget: Optional[LinkBudget] = None,
                 parent=None):
        super().__init__(parent)
        self._tel_port = tel_port
        self._cmd_port = cmd_port
        self._priority = max(0, min(999, int(priority)))
        self._disc_port = discovery_port
        self._sent_nonces = sent_nonces
        self._rounds = DiscoveryRounds(budget if budget is not None else ground_budget(),
                                       interval_s, healthy_interval_s)
        self._targets: list[str] = []
        self._targets_at: Optional[float] = None
        self._stop = threading.Event()
        # Radio silence (spec §9): while quiet the thread keeps running but
        # sends nothing, so RADIO_RESUME brings discovery back instantly.
        self._quiet = threading.Event()
        # Telemetry arrived within the last 5 s (MainWindow decides).
        self._healthy = threading.Event()
        self.beacons_sent = 0
        self.hellos_sent = 0

    @property
    def datagrams_skipped(self) -> int:
        return self._rounds.skipped

    def set_priority(self, priority: int) -> None:
        self._priority = max(0, min(999, int(priority)))

    def set_quiet(self, quiet: bool) -> None:
        if quiet:
            self._quiet.set()
        else:
            self._quiet.clear()

    def is_quiet(self) -> bool:
        return self._quiet.is_set()

    def set_link_healthy(self, healthy: bool) -> None:
        if healthy:
            self._healthy.set()
        else:
            self._healthy.clear()

    def is_link_healthy(self) -> bool:
        return self._healthy.is_set()

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
            while not self._stop.is_set():
                now = time.monotonic()
                self.poll(sock, now)
                wait = (self._CHECK_S if self._quiet.is_set()
                        else self._rounds.next_check_s(now, self._healthy.is_set()))
                self._stop.wait(min(self._CHECK_S, max(0.01, wait)))
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def poll(self, sock, now: float) -> None:
        """One check: start a round when one is due, then send what the link
        budget lets out. `now` is monotonic seconds (tests pass their own)."""
        if self._quiet.is_set():
            self._rounds.clear()
            return
        healthy = self._healthy.is_set()
        if self._rounds.due(now, healthy):
            if self._targets_at is None or now - self._targets_at > self._TARGET_REFRESH_S:
                self._targets = _discovery_targets()
                self._targets_at = now
            nonce = str(int(time.time() * 1000))
            # Register before the first sendto: the loopback copy can
            # reach our own listener before the send loop finishes.
            if self._sent_nonces is not None:
                self._sent_nonces.add(nonce)
            line = (
                f"GS_BEACON,{nonce},{self._tel_port},"
                f"{self._cmd_port},{self._priority}\n"
            ).encode("utf-8")
            datagrams = [(line, addr) for addr in self._targets]
            if not healthy:
                # also legacy GS_HELLO for older onboard builds
                legacy = (
                    f"GS_HELLO,{nonce},{self._tel_port},{self._cmd_port}\n"
                ).encode("utf-8")
                datagrams += [(legacy, addr) for addr in self._targets]
            self._rounds.start(now, healthy, datagrams)
        self._rounds.send_pending(healthy, lambda data, addr: self._send(sock, data, addr))

    def _send(self, sock, data: bytes, addr: str) -> None:
        try:
            sock.sendto(data, (addr, self._disc_port))
        except OSError:
            return
        if data.startswith(b"GS_BEACON,"):
            self.beacons_sent += 1
        else:
            self.hellos_sent += 1


# ── listener for onboard + peer GS announcements ─────────────────────────────
class OnboardListener(QThread):
    """Listens on UDP 0.0.0.0:<discovery_port> for announcements."""

    onboard_discovered = pyqtSignal(str, int, int, str, str)
    # (host, cmd_port, tel_port, session_id, hostname)
    peer_gs_seen = pyqtSignal(str, int)  # (host, priority)
    log_message = pyqtSignal(str)

    _DEDUP_WINDOW_S = 2.0
    # A peer beacons every ~2 s; one log line per sighting floods the event
    # log (the old dedup window equalled the beacon interval, so it never
    # suppressed anything). Re-report an unchanged peer at most this often.
    _PEER_RESEEN_S = 300.0

    def __init__(self, discovery_port: int = DISCOVERY_PORT_DEFAULT,
                 sent_nonces: Optional[SentNonceRegistry] = None,
                 parent=None):
        super().__init__(parent)
        self._port = discovery_port
        self._sent_nonces = sent_nonces
        self._stop = threading.Event()
        self._last_onboard: tuple = ()
        self._last_onboard_t: float = 0.0
        self._peer_throttle = PeerSightingThrottle(self._PEER_RESEEN_S)

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
            # Our own broadcasts loop back to this listener; a ground
            # station is not a conflicting peer of itself.
            if self._sent_nonces is not None and \
                    self._sent_nonces.was_sent(peer["nonce"]):
                return
            if not self._peer_throttle.should_emit(src_host, peer["priority"],
                                                   time.monotonic()):
                return
            self.peer_gs_seen.emit(src_host, peer["priority"])


class CommandProbe(QThread):
    """Low-rate TCP command probe for deterministic link-local bring-up.

    Probes only while no telemetry arrives (docs/link-budget.md). Each PING
    exchange is held on the ground station's link budget, at discovery
    priority, for the connection's duration; a probe the budget cannot take
    at once is not waited for but retried shortly, with the same host."""

    onboard_reachable = pyqtSignal(str, int)
    log_message = pyqtSignal(str)

    _PING = b"PING\n"
    _BUDGET_RETRY_S = DiscoveryRounds.RETRY_S

    def __init__(self, hosts: list[str], cmd_port: int,
                 interval_s: float = DISCOVERY_INTERVAL_S,
                 timeout_s: float = 0.7,
                 include_static: bool = True,
                 budget: Optional[LinkBudget] = None,
                 parent=None):
        super().__init__(parent)
        self._include_static = include_static
        self._hosts = probe_host_candidates(*hosts, include_static=include_static)
        self._cmd_port = int(cmd_port)
        self._interval = float(interval_s)
        self._timeout = float(timeout_s)
        self._budget = budget if budget is not None else ground_budget()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._last_success: tuple[str, int] | None = None
        # Radio silence: every probe makes the onboard answer, so the probe
        # is parked while silent and resumes on RADIO_RESUME.
        self._quiet = threading.Event()
        # Telemetry arrived within the last 5 s: the onboard is found.
        self._healthy = threading.Event()
        self._round: Optional[list[str]] = None   # hosts still to probe this round
        self.probes_sent = 0
        self.budget_skips = 0

    def set_candidates(self, hosts: list[str]) -> None:
        with self._lock:
            self._hosts = probe_host_candidates(*hosts,
                                                include_static=self._include_static)

    def set_quiet(self, quiet: bool) -> None:
        if quiet:
            self._quiet.set()
        else:
            self._quiet.clear()

    def is_quiet(self) -> bool:
        return self._quiet.is_set()

    def set_link_healthy(self, healthy: bool) -> None:
        if healthy:
            self._healthy.set()
        else:
            self._healthy.clear()

    def is_link_healthy(self) -> bool:
        return self._healthy.is_set()

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        while not self._stop.is_set():
            if self._quiet.is_set() or self._healthy.is_set():
                self._round = None
                self._stop.wait(self._interval)
                continue
            finished = self.probe_round()
            self._stop.wait(self._interval if finished else self._BUDGET_RETRY_S)

    def probe_round(self) -> bool:
        """Probe the candidates in order until one answers. False when the
        link budget cannot take the next probe now; the next call resumes the
        round with that host."""
        if self._round is None:
            with self._lock:
                self._round = list(self._hosts)
        while self._round and not self._stop.is_set():
            host = self._round[0]
            ticket = self._budget.hold(command_exchange_bytes(len(self._PING)), Priority.DISCOVERY)
            if ticket is None:
                self.budget_skips += 1
                return False
            reachable = self._try_ping(host, ticket)
            self._round.pop(0)
            if reachable:
                key = (host, self._cmd_port)
                if key != self._last_success:
                    self.log_message.emit(f"[discovery] command probe {host}:{self._cmd_port}")
                    self._last_success = key
                self.onboard_reachable.emit(host, self._cmd_port)
                break
        self._round = None
        return True

    def _try_ping(self, host: str, ticket) -> bool:
        """One PING exchange; `paced_connection` releases `ticket` once the
        connection is closed."""
        self.probes_sent += 1
        try:
            with paced_connection(self._budget, ticket, host, self._cmd_port, self._timeout) as sock:
                sock.settimeout(self._timeout)
                sock.sendall(self._PING)
                data = sock.recv(256)
        except OSError:
            return False
        return data.decode("utf-8", errors="replace").strip().startswith("ACK,PING")
