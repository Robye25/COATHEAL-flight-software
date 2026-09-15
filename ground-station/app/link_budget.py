"""The ground station's share of the 24 kbps E-Link (docs/link-budget.md).

Owner rule, 2026-09-15: all COATHEAL traffic on the E-Link stays at or below
24 000 bit/s in every 1-second window, both directions, every byte on the
wire. Neither side sees the other's traffic in time to react, so the cap is
split into fixed shares: the onboard enforces 1 600 B (its frames and the
ground station's automatic ACK replies), the ground station 1 150 B (command
exchanges, discovery beacons and probes), and 250 B stay unaccounted.

`LinkBudget` is the ledger. A charge is placed before its bytes can reach the
wire and counts until `window_s` after it is released; an open hold counts
for as long as it is open. A sender goes only when the bytes counting now
plus its charge fit the share, and never takes bytes a waiting sender of
higher priority is queued for. Charging at or before emission and releasing
after the last byte is what turns "the ledger never exceeds the share" into
"no 1-second window on the wire does". No Qt here.
"""
from __future__ import annotations

import contextlib
import enum
import math
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from typing import Callable, Iterator, List, Optional, Tuple

# ── byte model (on-wire bytes) ───────────────────────────────────────────────
WIRE_OVERHEAD = 24       # preamble/SFD, FCS and inter-frame gap
TCP_HEADERS = 66         # Ethernet 14 + IPv4 20 + TCP 32 (Linux sends the timestamp option)
TCP_MAX_PAYLOAD = 1448
UDP_HEADERS = 42         # Ethernet 14 + IPv4 20 + UDP 8
MIN_FRAME = 60           # Ethernet minimum, FCS excluded
PURE_ACK = 90            # also a FIN
SYN = 98                 # also a SYN-ACK


def tcp_segment(payload_bytes: int) -> int:
    """A TCP send of `payload_bytes`; more than 1 448 B is split into
    segments that each carry their own headers."""
    full, rest = divmod(max(0, int(payload_bytes)), TCP_MAX_PAYLOAD)
    total = full * (TCP_HEADERS + TCP_MAX_PAYLOAD + WIRE_OVERHEAD)
    if rest or not full:
        total += TCP_HEADERS + rest + WIRE_OVERHEAD
    return total


def udp_datagram(payload_bytes: int) -> int:
    return max(MIN_FRAME, UDP_HEADERS + int(payload_bytes)) + WIRE_OVERHEAD


# ── shares ───────────────────────────────────────────────────────────────────
CAP_BYTES = 3000         # 24 000 bit/s
ONBOARD_SHARE = 1600
GROUND_SHARE = 1150
UNACCOUNTED = 250
WINDOW_S = 1.0

# One command exchange without its request payload: SYN, SYN-ACK, ACK, the
# request segment's headers, the onboard's ACK, the reply segment's headers,
# FIN, ACK, FIN, ACK (916 B, rounded up). The reply payload and our ACK of it
# are charged onboard.
COMMAND_EXCHANGE_FIXED = 920
# After a clean exchange the onboard's closing FIN or ACK, and our kernel's
# ACK of it, are still on their way when the socket is closed: the hold is
# released this long plus two round trips after the close.
CLOSE_TAIL_S = 0.05
# After a reset (or when the round trip is unknown) segments the onboard sent
# before the reset reached it draw resets from our kernel: the hold stays this
# long plus one round trip, as onboard (wire::kAbortTail).
ABORT_TAIL_S = 0.5
# The FIN and the onboard's ACK of it, charged before the ground station
# closes a telemetry connection that went quiet.
TELEMETRY_CLOSE_BYTES = 2 * PURE_ACK
# The longest request line (newline included) whose exchange fits the share.
MAX_REQUEST_BYTES = GROUND_SHARE - COMMAND_EXCHANGE_FIXED          # 230
# How long a command waits for room on the ledger before it fails locally
# (its own timeout when that is longer). Exchanges go one at a time, so this
# covers the queue ahead of it.
COMMAND_BUDGET_WAIT_S = 10.0

# ── discovery cadence ────────────────────────────────────────────────────────
LINK_HEALTHY_S = 5.0                  # a telemetry frame this recent: the link is up
DISCOVERY_INTERVAL_S = 2.0            # GS_BEACON, GS_HELLO and PING while it is not
DISCOVERY_HEALTHY_INTERVAL_S = 15.0   # GS_BEACON only, while telemetry arrives


class Priority(enum.IntEnum):
    """Lower is more urgent."""
    CRITICAL = 0     # safety commands
    COMMAND = 1      # every other command
    POLL = 2         # background polls (the dispatcher's quiet sends)
    DISCOVERY = 3    # GS_BEACON, GS_HELLO, the PING probe


SAFETY_VERBS = frozenset({"HEATERS_OFF", "STEPPER_STOP", "DISARM", "SHUTDOWN_SAFE",
                          "RADIO_SILENCE", "RADIO_RESUME"})


def priority_for(command: str, quiet: bool = False) -> Priority:
    stripped = (command or "").strip()
    verb = stripped.split()[0].upper() if stripped else ""
    if verb in SAFETY_VERBS:
        return Priority.CRITICAL
    return Priority.POLL if quiet else Priority.COMMAND


def command_exchange_bytes(request_bytes: int) -> int:
    """The hold for one command exchange whose request line (newline
    included) is `request_bytes` long."""
    return COMMAND_EXCHANGE_FIXED + int(request_bytes)


def request_too_long(request_bytes: int) -> Optional[str]:
    """The local refusal for a request line that can never fit the share, or
    None when it can."""
    if request_bytes > MAX_REQUEST_BYTES:
        return (f"request too long for the 24 kbps link budget "
                f"({request_bytes} > {MAX_REQUEST_BYTES} B)")
    return None


def command_budget_wait_s(timeout_s: float) -> float:
    return max(COMMAND_BUDGET_WAIT_S, float(timeout_s))


def budget_wait_error(wait_s: float) -> str:
    return f"waited {wait_s:g} s for link budget"


def tcp_srtt_s(sock: Optional[socket.socket]) -> Optional[float]:
    """The connection's smoothed round trip in seconds (Linux TCP_INFO), or
    None where the kernel does not report it."""
    option = getattr(socket, "TCP_INFO", None)
    if sock is None or option is None:
        return None
    try:
        info = sock.getsockopt(socket.IPPROTO_TCP, option, 104)
    except (OSError, ValueError):
        return None
    if len(info) < 72:
        return None
    (rtt_us,) = struct.unpack_from("I", info, 68)   # tcpi_rtt
    return rtt_us / 1e6


def reset_on_close(sock: socket.socket) -> None:
    """Make the next close() send a reset instead of a FIN (SO_LINGER 0)."""
    linger = struct.pack("HH" if sys.platform == "win32" else "ii", 1, 0)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
    except OSError:
        pass


@contextlib.contextmanager
def paced_connection(budget: "LinkBudget", charge: "Charge", host: str, port: int,
                     timeout_s: float) -> Iterator[socket.socket]:
    """The command connection for the exchange `charge` holds; the hold is
    released when the connection closes. A clean exchange closes normally and
    releases CLOSE_TAIL_S plus two round trips later. One that raises is
    closed with a reset -- so the onboard can no longer answer it -- and the
    hold stays ABORT_TAIL_S plus a round trip."""
    sock: Optional[socket.socket] = None
    clean = False
    try:
        sock = socket.create_connection((host, port), timeout=timeout_s)
        yield sock
        clean = True
    finally:
        srtt = tcp_srtt_s(sock)
        if sock is not None:
            if not clean:
                reset_on_close(sock)
            try:
                sock.close()
            except OSError:
                pass
        tail = (CLOSE_TAIL_S + 2 * srtt) if (clean and srtt is not None) else ABORT_TAIL_S + (srtt or 0.0)
        budget.release(charge, tail)


@dataclass(eq=False)
class Charge:
    """One ledger entry (the ticket `try_charge`/`hold` return). It counts
    from `placed_at` until `released_at + window_s`; `released_at` is
    infinite while a hold is open."""
    nbytes: int
    priority: Priority
    placed_at: float
    released_at: float

    @property
    def is_open(self) -> bool:
        return math.isinf(self.released_at)


@dataclass(eq=False)
class _Waiter:
    priority: Priority
    order: int


class LinkBudget:
    """Thread-safe sliding-window ledger of on-wire bytes.

    `clock` is injectable: a test drives a fake clock and calls `notify()` so
    waiting holds re-check the ledger without sleeping.
    """

    # A cancellable wait re-checks its event at least this often.
    CANCEL_POLL_S = 0.1

    def __init__(self, share_bytes: int, window_s: float = WINDOW_S,
                 clock: Callable[[], float] = time.monotonic):
        self.share_bytes = int(share_bytes)
        self.window_s = float(window_s)
        self._clock = clock
        self._cond = threading.Condition()
        self._charges: List[Charge] = []
        self._waiters: List[_Waiter] = []
        self._orders = 0

    # -- queries ---------------------------------------------------------------
    def in_window(self) -> int:
        """Bytes counting against the share right now."""
        with self._cond:
            return self._counting(self._clock())

    def waiting(self) -> Tuple[Priority, ...]:
        """Priorities of the holds waiting for room, in arrival order."""
        with self._cond:
            return tuple(w.priority for w in self._waiters)

    # -- charges ---------------------------------------------------------------
    def try_charge(self, nbytes: int, priority: Priority) -> Optional[Charge]:
        """Charge `nbytes` now, released at once (it counts for `window_s`),
        or None when it does not fit or a higher-priority hold is waiting."""
        with self._cond:
            now = self._clock()
            if not self._fits(nbytes, priority, now, None):
                return None
            return self._place(nbytes, priority, now, released=True)

    def hold(self, nbytes: int, priority: Priority, timeout_s: float = 0.0,
             cancel: Optional[threading.Event] = None) -> Optional[Charge]:
        """Open a hold of `nbytes`, waiting up to `timeout_s` for room behind
        every waiting hold of higher priority and every earlier one of the
        same priority. It counts until `release()` plus `window_s`. None on
        timeout, on `cancel`, and at once for more than the share."""
        with self._cond:
            if nbytes > self.share_bytes:
                return None
            now = self._clock()
            waiter = _Waiter(priority, self._orders)
            self._orders += 1
            if self._fits(nbytes, priority, now, waiter):
                return self._place(nbytes, priority, now, released=False)
            if timeout_s <= 0 or (cancel is not None and cancel.is_set()):
                return None
            deadline = now + timeout_s
            self._waiters.append(waiter)
            try:
                while True:
                    self._cond.wait(self._wait_s(now, deadline, cancel))
                    if cancel is not None and cancel.is_set():
                        return None
                    now = self._clock()
                    if self._fits(nbytes, priority, now, waiter):
                        return self._place(nbytes, priority, now, released=False)
                    if now >= deadline:
                        return None
            finally:
                self._waiters.remove(waiter)
                # A lower-priority or later hold may have been waiting on this one.
                self._cond.notify_all()

    def release(self, charge: Optional[Charge], delay_s: float = 0.0) -> None:
        """Close a hold `delay_s` from now (while the last bytes it covers may
        still be on their way); it keeps counting for `window_s` after that.
        A plain charge (or a hold released before) is left as it is."""
        if charge is None:
            return
        with self._cond:
            if charge.is_open:
                charge.released_at = self._clock() + max(0.0, float(delay_s))
            self._cond.notify_all()

    def notify(self) -> None:
        """Wake every waiting hold to re-check the ledger (after a fake clock
        moved)."""
        with self._cond:
            self._cond.notify_all()

    # -- internals (lock held) ---------------------------------------------------
    def _counting(self, now: float) -> int:
        self._charges = [c for c in self._charges if c.released_at + self.window_s > now]
        return sum(c.nbytes for c in self._charges)

    def _fits(self, nbytes: int, priority: Priority, now: float, waiter: Optional[_Waiter]) -> bool:
        if nbytes > self.share_bytes or self._counting(now) + nbytes > self.share_bytes:
            return False
        for other in self._waiters:
            if other is waiter:
                continue
            if other.priority < priority:
                return False
            if waiter is not None and other.priority == priority and other.order < waiter.order:
                return False
        return True

    def _place(self, nbytes: int, priority: Priority, now: float, *, released: bool) -> Charge:
        charge = Charge(int(nbytes), priority, now, now if released else math.inf)
        self._charges.append(charge)
        return charge

    def _wait_s(self, now: float, deadline: float, cancel: Optional[threading.Event]) -> float:
        """Until the earliest of: the deadline, the next charge ageing out."""
        wake = deadline
        for charge in self._charges:
            expiry = charge.released_at + self.window_s
            if now < expiry < wake:
                wake = expiry
        wait = max(0.0, wake - now)
        return min(wait, self.CANCEL_POLL_S) if cancel is not None else wait


class DiscoveryRounds:
    """When a discovery round is due, and which of its datagrams are still to
    go (docs/link-budget.md, "Discovery cadence"). Not thread-safe: one
    sender thread owns it.

    A round is due every `interval_s` while the link is down and every
    `healthy_interval_s` while telemetry arrives. Each datagram is charged
    (`udp_datagram`, DISCOVERY) right before it is sent; one the ledger
    cannot take is skipped and tried again at the sender's next check until
    the next round replaces it. Without the retry two 2 s senders on one
    1 150 B share (the beacon and the PING probe) lock phase and one of them
    never gets out.
    """

    RETRY_S = 0.25

    def __init__(self, budget: LinkBudget, interval_s: float = DISCOVERY_INTERVAL_S,
                 healthy_interval_s: float = DISCOVERY_HEALTHY_INTERVAL_S):
        self.budget = budget
        self.interval_s = float(interval_s)
        self.healthy_interval_s = float(healthy_interval_s)
        self.skipped = 0          # datagrams the ledger refused (each refusal counts)
        self._last_round: Optional[float] = None
        self._round_healthy: Optional[bool] = None
        self._pending: List[Tuple[bytes, object]] = []

    @property
    def pending(self) -> int:
        return len(self._pending)

    def due(self, now: float, healthy: bool) -> bool:
        if self._last_round is None:
            return True
        return now - self._last_round >= (self.healthy_interval_s if healthy else self.interval_s)

    def start(self, now: float, healthy: bool, datagrams: List[Tuple[bytes, object]]) -> None:
        """Begin a round; whatever the previous one still had pending is dropped."""
        self._last_round = now
        self._round_healthy = healthy
        self._pending = list(datagrams)

    def clear(self) -> None:
        self._pending = []

    def send_pending(self, healthy: bool, send: Callable[[bytes, object], None]) -> None:
        """Charge and `send(data, address)` every pending datagram the ledger
        takes now. A round begun in the other link state is dropped first
        (no legacy hello once telemetry arrives)."""
        if healthy != self._round_healthy:
            self._pending = []
        still: List[Tuple[bytes, object]] = []
        for data, address in self._pending:
            if self.budget.try_charge(udp_datagram(len(data)), Priority.DISCOVERY) is None:
                self.skipped += 1
                still.append((data, address))
                continue
            send(data, address)
        self._pending = still

    def next_check_s(self, now: float, healthy: bool) -> float:
        """How long the sender may sleep before its next check."""
        if self._pending:
            return self.RETRY_S
        if self._last_round is None:
            return 0.0
        interval = self.healthy_interval_s if healthy else self.interval_s
        return max(0.0, self._last_round + interval - now)


_GROUND_BUDGET = LinkBudget(GROUND_SHARE)


def ground_budget() -> LinkBudget:
    """The one ledger of this ground-station process: the command dispatcher,
    the discovery beacon and the command probe all charge it."""
    return _GROUND_BUDGET
