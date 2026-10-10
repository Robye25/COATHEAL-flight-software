"""The onboard's clock, set from this ground station (`TIME_SYNC`).

The BEXUS E-Link carries no NTP and the Pi has no RTC, so this ground
station is the onboard's only time reference. It sends
`TIME_SYNC <ground_unix_ms> [<rtt_ms>]` once per onboard session as soon as
telemetry arrives, then every SYNC_PERIOD_S, as a quiet background exchange
at poll priority that never waits for the link budget: when the budget has
no room the attempt is skipped and tried again RETRY_S later. The onboard
answers `offset_ms=<ground - onboard>;applied=<0|1>;rtt_ms=<used>;syncs=<n>;
now=<its clock>` and steps its clock when the offset reaches its threshold
(onboard config clock.step_threshold_ms, 250 ms by default). This module is
the pure part: schedule, command, reply, status line. No Qt.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Optional, Tuple

from .link_budget import BUDGET_FULL_ERROR

SYNC_PERIOD_S = 600.0        # one exchange (~1 KB on the ledger) every ten minutes
RETRY_S = 30.0               # after a skipped (budget full) or unanswered attempt
FAILED_RETRY_S = 300.0       # after a NACK (the onboard refused or could not set its clock)


def parse_reply(body: str) -> Dict[str, str]:
    """`offset_ms=-1234;applied=1;rtt_ms=120;syncs=3;now=...` as a dict."""
    fields: Dict[str, str] = {}
    for item in (body or "").split(";"):
        key, sep, value = item.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    return fields


def format_offset(offset_ms: int) -> str:
    if abs(offset_ms) >= 1000:
        return f"{offset_ms / 1000.0:+.1f} s"
    return f"{offset_ms:+d} ms"


def format_age(seconds: float) -> str:
    seconds = max(0.0, seconds)
    if seconds < 60:
        return f"{seconds:.0f} s"
    if seconds < 3600:
        return f"{seconds / 60:.0f} min"
    return f"{seconds / 3600:.1f} h"


@dataclass
class ClockSync:
    period_s: float = SYNC_PERIOD_S
    retry_s: float = RETRY_S
    failed_retry_s: float = FAILED_RETRY_S
    session: str = ""
    pending: bool = False
    next_mono: Optional[float] = None       # None: nothing to sync (no session yet)
    last_rtt_ms: float = 0.0
    last_offset_ms: Optional[int] = None
    last_applied: Optional[bool] = None
    last_check_mono: Optional[float] = None
    last_error: Optional[str] = None
    checks: int = 0

    # -- schedule ------------------------------------------------------------------
    def on_session(self, session_id: str, now_mono: float) -> None:
        """A new onboard session (a reboot: its clock is fake-hwclock's save
        again) is synced at once; the first session too."""
        if session_id and session_id != self.session:
            self.session = session_id
            self.next_mono = now_mono
            self.last_offset_ms = None
            self.last_applied = None
            self.last_error = None
            self.pending = False

    def due(self, now_mono: float, link_ok: bool, silent: bool) -> bool:
        return (bool(self.session) and link_ok and not silent and not self.pending
                and self.next_mono is not None and now_mono >= self.next_mono)

    def command(self, now_unix_s: float) -> str:
        """The line to send now; the previous exchange's half latency is the
        round-trip hint (connect handshake plus request and reply are two
        round trips)."""
        self.pending = True
        ground_ms = int(round(now_unix_s * 1000.0))
        rtt_ms = int(round(self.last_rtt_ms))
        return f"TIME_SYNC {ground_ms} {rtt_ms}" if rtt_ms > 0 else f"TIME_SYNC {ground_ms}"

    # -- reply ----------------------------------------------------------------------
    def on_reply(self, ok: bool, body: str, latency_ms: float, now_mono: float) -> Tuple[str, str]:
        """Absorb the reply (or the local refusal) and schedule the next
        attempt. Returns (event line, level)."""
        self.pending = False
        if ok:
            fields = parse_reply(body)
            try:
                offset = int(fields.get("offset_ms", ""))
            except ValueError:
                self.last_error = f"unreadable reply: {body}"
                self.next_mono = now_mono + self.retry_s
                return f"[clock] unreadable TIME_SYNC reply: {body}", "WARN"
            self.checks += 1
            self.last_check_mono = now_mono
            self.last_offset_ms = offset
            self.last_applied = fields.get("applied") == "1"
            self.last_error = None
            self.last_rtt_ms = max(0.0, latency_ms / 2.0)
            self.next_mono = now_mono + self.period_s
            if self.last_applied:
                return (f"[clock] onboard clock was {format_offset(-offset)} off; stepped to ground time "
                        f"(offset {format_offset(offset)}, {latency_ms:.0f} ms exchange)"), "WARN"
            return f"[clock] onboard clock in sync: offset {format_offset(offset)} ({latency_ms:.0f} ms exchange)", "INFO"
        error = body or "no reply"
        if BUDGET_FULL_ERROR in error:
            self.next_mono = now_mono + self.retry_s
            return f"[clock] TIME_SYNC postponed {self.retry_s:.0f} s: {error}", "INFO"
        self.last_error = error
        self.last_check_mono = now_mono
        refused = error.startswith("offset_ms=") or "disabled" in error or "implausible" in error
        self.next_mono = now_mono + (self.failed_retry_s if refused else self.retry_s)
        return f"[clock] TIME_SYNC failed: {error}", "WARN"

    # -- status --------------------------------------------------------------------------
    def status(self, now_mono: float) -> Tuple[str, str]:
        """(text, colour key) for the System tab: green in sync, amber just
        stepped, red failed, muted not checked."""
        if self.last_error and self.last_check_mono is not None:
            return f"sync failed: {self.last_error}", "red"
        if self.last_offset_ms is None or self.last_check_mono is None:
            return ("checking…" if self.pending else "not checked"), "muted"
        age = format_age(now_mono - self.last_check_mono)
        if self.last_applied:
            return f"stepped {format_offset(self.last_offset_ms)} · {age} ago", "amber"
        return f"in sync ({format_offset(self.last_offset_ms)}) · checked {age} ago", "green"
