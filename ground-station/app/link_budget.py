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

Hard cap: the kernel of each side also shapes everything that side sends on
its E-Link port with a token bucket (`scripts/link_cap.sh`), so that no bug,
retransmission or other program can put more on the wire. A bucket of
`burst` bytes refilled at `rate` bytes per second passes at most burst + rate
bytes in any second: onboard 1 000 + 800, ground 700 + 500, 3 000 B in all.
`LinkBudget.set_egress_shaper` makes the ledger model the bucket on this
side's port, so that what it lets out never has to wait there.
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
# The E-Link port runs at this MTU while the hard cap is installed, so that no
# frame is larger than a shaper's bucket.
LINK_MTU = 576
TCP_MAX_PAYLOAD = LINK_MTU - 52   # 524: IPv4 20 + TCP 32 (timestamps)
UDP_HEADERS = 42         # Ethernet 14 + IPv4 20 + UDP 8
MIN_FRAME = 60           # Ethernet minimum, FCS excluded
PURE_ACK = 90            # also a FIN
SYN = 98                 # also a SYN-ACK


def tcp_segment(payload_bytes: int) -> int:
    """A TCP send of `payload_bytes`; more than one segment's worth (524 B
    at the capped MTU) is split into segments that each carry their own
    headers."""
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

# ── hard cap: the kernel shapers (scripts/link_cap.sh) ───────────────────────
# Token bucket per side, in on-wire bytes: bucket + one second of rate bounds
# every 1-second window of what that side sends.
ONBOARD_EGRESS_BURST = 1000
ONBOARD_EGRESS_RATE = 800          # bytes per second
GROUND_EGRESS_BURST = 700
GROUND_EGRESS_RATE = 500
# The ledger's model of the bucket refills this much slower than the kernel's
# (clock drift, the few frames nobody schedules).
EGRESS_MODEL_RATE_PERCENT = 97

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
# What this side sends for one command exchange without its request payload:
# SYN, ACK, the request segment's headers, FIN, ACK.
COMMAND_EXCHANGE_EGRESS_FIXED = SYN + 4 * PURE_ACK                 # 458
# Tokens a command leaves in the modelled bucket: the ACK lines of one tick's
# telemetry (the live frame and a replayed one) must not wait in the shaper,
# or the onboard's 180 ms ACK deadline resets the link.
ACK_RESERVE_BYTES = 2 * (TCP_HEADERS + 40 + WIRE_OVERHEAD)         # 260
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


def command_exchange_egress(request_bytes: int) -> int:
    """The part of `command_exchange_bytes` this side sends: what the
    exchange takes from the modelled egress shaper."""
    return COMMAND_EXCHANGE_EGRESS_FIXED + int(request_bytes)


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
    hold stays ABORT_TAIL_S plus a round trip. The round trip is the kernel's
    smoothed estimate where it reports one (Linux TCP_INFO), else the time
    the connect handshake took (Windows), which is one round trip."""
    sock: Optional[socket.socket] = None
    clean = False
    connect_rtt: Optional[float] = None
    try:
        started = time.monotonic()
        sock = socket.create_connection((host, port), timeout=timeout_s)
        connect_rtt = max(0.0, time.monotonic() - started)
        yield sock
        clean = True
    finally:
        srtt = tcp_srtt_s(sock)
        if srtt is None and sock is not None:
            srtt = connect_rtt
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
        # Model of the kernel shaper on this side's port (0 burst: none).
        self._egress_burst = 0.0
        self._egress_rate = 0.0
        self._egress_reserve = 0.0
        self._egress_tokens = 0.0
        self._egress_stamp = self._clock()

    # -- queries ---------------------------------------------------------------
    def in_window(self) -> int:
        """Bytes counting against the share right now."""
        with self._cond:
            return self._counting(self._clock())

    def waiting(self) -> Tuple[Priority, ...]:
        """Priorities of the holds waiting for room, in arrival order."""
        with self._cond:
            return tuple(w.priority for w in self._waiters)

    # -- the kernel shaper on this side's port -------------------------------------
    def set_egress_shaper(self, burst_bytes: int, rate_bytes_per_s: float,
                          reserve_bytes: int = 0) -> None:
        """Model a kernel token bucket of `burst_bytes`, refilled at
        `rate_bytes_per_s`, on this side's E-Link port. From then on a charge
        goes only while the bucket holds its `tx_bytes` -- and, below
        CRITICAL priority, `reserve_bytes` on top of them, kept for what
        cannot wait (`debit_egress`). A charge that needs more than the
        bucket goes when the bucket is full."""
        with self._cond:
            self._egress_burst = float(burst_bytes)
            self._egress_rate = float(rate_bytes_per_s)
            self._egress_reserve = float(reserve_bytes)
            self._egress_tokens = float(burst_bytes)
            self._egress_stamp = self._clock()
            self._cond.notify_all()

    @property
    def egress_shaped(self) -> bool:
        return self._egress_burst > 0

    def egress_tokens(self) -> float:
        """Tokens in the modelled bucket now (negative while in debt)."""
        with self._cond:
            self._refill(self._clock())
            return self._egress_tokens

    def debit_egress(self, nbytes: int) -> None:
        """Bytes this side sends that no charge waited for (a telemetry ACK
        line, the kernel's answer to an incoming connection). The bucket may
        go into debt, by at most one bucket; later charges wait until it is
        paid back."""
        with self._cond:
            if self._egress_burst <= 0:
                return
            self._refill(self._clock())
            # The kernel's bucket is never emptier than empty (what does not
            # fit waits in its queue or is dropped), so the model's debt is
            # bounded too: one bucket.
            self._egress_tokens = max(self._egress_tokens - int(nbytes), -self._egress_burst)

    def refund_egress(self, nbytes: int) -> None:
        """Give back `tx_bytes` of a charge that were never sent."""
        with self._cond:
            if self._egress_burst <= 0:
                return
            self._refill(self._clock())
            self._egress_tokens = min(self._egress_burst, self._egress_tokens + int(nbytes))
            self._cond.notify_all()

    # -- charges ---------------------------------------------------------------
    def try_charge(self, nbytes: int, priority: Priority, tx_bytes: int = 0) -> Optional[Charge]:
        """Charge `nbytes` now, released at once (it counts for `window_s`),
        or None when it does not fit or a higher-priority hold is waiting.
        `tx_bytes` of them are sent by this side (`set_egress_shaper`)."""
        with self._cond:
            now = self._clock()
            if not self._fits(nbytes, priority, now, None, tx_bytes):
                return None
            return self._place(nbytes, priority, now, released=True, tx_bytes=tx_bytes)

    def hold(self, nbytes: int, priority: Priority, timeout_s: float = 0.0,
             cancel: Optional[threading.Event] = None, tx_bytes: int = 0) -> Optional[Charge]:
        """Open a hold of `nbytes`, waiting up to `timeout_s` for room behind
        every waiting hold of higher priority and every earlier one of the
        same priority. It counts until `release()` plus `window_s`. None on
        timeout, on `cancel`, and at once for more than the share.
        `tx_bytes` of them are sent by this side (`set_egress_shaper`)."""
        with self._cond:
            if nbytes > self.share_bytes:
                return None
            now = self._clock()
            waiter = _Waiter(priority, self._orders)
            self._orders += 1
            if self._fits(nbytes, priority, now, waiter, tx_bytes):
                return self._place(nbytes, priority, now, released=False, tx_bytes=tx_bytes)
            if timeout_s <= 0 or (cancel is not None and cancel.is_set()):
                return None
            deadline = now + timeout_s
            self._waiters.append(waiter)
            try:
                while True:
                    self._cond.wait(self._wait_s(now, deadline, cancel, priority, tx_bytes))
                    if cancel is not None and cancel.is_set():
                        return None
                    now = self._clock()
                    if self._fits(nbytes, priority, now, waiter, tx_bytes):
                        return self._place(nbytes, priority, now, released=False, tx_bytes=tx_bytes)
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

    def _refill(self, now: float) -> None:
        if now > self._egress_stamp:
            self._egress_tokens = min(self._egress_burst,
                                      self._egress_tokens + self._egress_rate * (now - self._egress_stamp))
            self._egress_stamp = now

    def _egress_need(self, priority: Priority, tx_bytes: int) -> float:
        """Tokens the bucket must hold before a charge of `tx_bytes` goes."""
        if self._egress_burst <= 0 or tx_bytes <= 0:
            return 0.0
        reserve = 0.0 if priority == Priority.CRITICAL else self._egress_reserve
        return min(float(tx_bytes) + reserve, self._egress_burst)

    def _fits(self, nbytes: int, priority: Priority, now: float, waiter: Optional[_Waiter],
              tx_bytes: int = 0) -> bool:
        if nbytes > self.share_bytes or self._counting(now) + nbytes > self.share_bytes:
            return False
        need = self._egress_need(priority, tx_bytes)
        if need > 0:
            self._refill(now)
            if self._egress_tokens < need:
                return False
        for other in self._waiters:
            if other is waiter:
                continue
            if other.priority < priority:
                return False
            if waiter is not None and other.priority == priority and other.order < waiter.order:
                return False
        return True

    def _place(self, nbytes: int, priority: Priority, now: float, *, released: bool,
               tx_bytes: int = 0) -> Charge:
        charge = Charge(int(nbytes), priority, now, now if released else math.inf)
        self._charges.append(charge)
        if self._egress_burst > 0 and tx_bytes > 0:
            self._refill(now)
            self._egress_tokens -= int(tx_bytes)
        return charge

    def _wait_s(self, now: float, deadline: float, cancel: Optional[threading.Event],
                priority: Priority = Priority.COMMAND, tx_bytes: int = 0) -> float:
        """Until the earliest of: the deadline, the next charge ageing out,
        the modelled bucket holding what the charge needs."""
        wake = deadline
        for charge in self._charges:
            expiry = charge.released_at + self.window_s
            if now < expiry < wake:
                wake = expiry
        need = self._egress_need(priority, tx_bytes)
        if need > 0 and self._egress_rate > 0:
            self._refill(now)
            if self._egress_tokens < need:
                wake = min(wake, now + (need - self._egress_tokens) / self._egress_rate + 1e-3)
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
            size = udp_datagram(len(data))
            if self.budget.try_charge(size, Priority.DISCOVERY, tx_bytes=size) is None:
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


def shaped_ground_budget() -> LinkBudget:
    """A ledger for the ground station's share that also models the ground
    station's kernel shaper (the hard cap), installed or not."""
    budget = LinkBudget(GROUND_SHARE)
    budget.set_egress_shaper(GROUND_EGRESS_BURST,
                             GROUND_EGRESS_RATE * EGRESS_MODEL_RATE_PERCENT / 100.0,
                             ACK_RESERVE_BYTES)
    return budget


def send_answer(budget: LinkBudget, conn: socket.socket, text: str) -> None:
    """Send a telemetry-connection answer (an ACK line, the HELLO answer).
    The onboard holds its bytes on its share; on this side's port they come
    out of the egress shaper, and they cannot wait: the onboard resets a link
    whose ACK misses its 180 ms deadline."""
    data = text.encode("utf-8")
    conn.sendall(data)
    budget.debit_egress(tcp_segment(len(data)))


_GROUND_BUDGET = shaped_ground_budget()


def ground_budget() -> LinkBudget:
    """The one ledger of this ground-station process: the command dispatcher,
    the discovery beacon and the command probe all charge it."""
    return _GROUND_BUDGET
