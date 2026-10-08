"""Whether the E-Link hard cap is on this machine (docs/link-budget.md,
"Hard cap").

The cap itself is a kernel shaper that `scripts/link_cap.sh` installs on the
E-Link port; this module only looks at what the kernel has, so the console
can say whether anything but the ledger limits what this station sends. No
Qt, no privilege: reading a queueing discipline is open to every user.
"""
from __future__ import annotations

import ipaddress
import platform
import re
import subprocess
from dataclasses import dataclass
from typing import Callable, Optional

from .link_budget import GROUND_EGRESS_BURST, GROUND_EGRESS_RATE, LINK_MTU, WIRE_OVERHEAD

Runner = Callable[[list], Optional[str]]


@dataclass(frozen=True)
class CapStatus:
    state: str            # "on" | "off" | "n/a" | "unknown"
    port: str             # the port that routes to the onboard, "" when not found
    detail: str           # one line for the event log

    @property
    def ok(self) -> bool:
        return self.state in ("on", "n/a")


def _run(cmd: list) -> Optional[str]:
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=3.0)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout if done.returncode == 0 else None


def _bits(text: str) -> Optional[int]:
    """tc's rate notation: "4Kbit", "6400bit", "1Mbit"."""
    match = re.fullmatch(r"(\d+)([KMG]?)bit", text)
    if match is None:
        return None
    return int(match.group(1)) * {"": 1, "K": 1000, "M": 1000_000, "G": 1000_000_000}[match.group(2)]


def _bytes(text: str) -> Optional[int]:
    """tc's size notation: "700b", "1Kb", "1000b/1"."""
    match = re.fullmatch(r"(\d+)(Kb|Mb|b)(?:/\d+)?", text)
    if match is None:
        return None
    return int(match.group(1)) * {"b": 1, "Kb": 1024, "Mb": 1024 * 1024}[match.group(2)]


def parse_tbf(qdisc_text: str) -> Optional[tuple]:
    """(rate bit/s, bucket bytes) of the root token bucket in `tc qdisc show`
    output, or None when the port has none."""
    for line in qdisc_text.splitlines():
        words = line.split()
        if len(words) < 3 or words[0] != "qdisc" or words[1] != "tbf" or "root" not in words:
            continue
        rate = _bits(words[words.index("rate") + 1]) if "rate" in words else None
        burst = _bytes(words[words.index("burst") + 1]) if "burst" in words else None
        if rate is not None and burst is not None:
            return rate, burst
    return None


def ground_cap_status(onboard_host: str, run: Runner = _run, system: Optional[str] = None) -> CapStatus:
    """The cap on the port this machine reaches `onboard_host` through."""
    system = system or platform.system()
    try:
        if ipaddress.ip_address(onboard_host).is_loopback:
            return CapStatus("n/a", "lo", "onboard on this machine (loopback): nothing reaches the E-Link")
    except ValueError:
        pass
    if system != "Linux":
        return CapStatus("unknown", "", f"no kernel shaper support for {system} here: only the ledger "
                                         "limits what this station sends (docs/link-budget.md, Hard cap)")
    route = run(["ip", "-o", "route", "get", onboard_host])
    match = re.search(r"\bdev (\S+)", route or "")
    if match is None:
        return CapStatus("unknown", "", f"no route to {onboard_host}: cannot tell which port the E-Link is on")
    port = match.group(1)
    tbf = parse_tbf(run(["tc", "qdisc", "show", "dev", port]) or "")
    link = run(["ip", "-o", "link", "show", "dev", port]) or ""
    mtu_match = re.search(r"\bmtu (\d+)", link)
    mtu = int(mtu_match.group(1)) if mtu_match else None
    want_bits = GROUND_EGRESS_RATE * 8
    # tc reports the bucket it keeps: the configured burst plus one frame's overhead.
    if (tbf is not None and tbf[0] == want_bits and tbf[1] <= GROUND_EGRESS_BURST + WIRE_OVERHEAD
            and mtu is not None and mtu <= LINK_MTU):
        return CapStatus("on", port, f"kernel shaper ON on {port}: {GROUND_EGRESS_RATE} B/s, bucket "
                                     f"{GROUND_EGRESS_BURST} B, MTU {mtu} (at most "
                                     f"{GROUND_EGRESS_RATE + GROUND_EGRESS_BURST} B in any second)")
    return CapStatus("off", port, f"NOT installed on {port}: only the ledger limits what this station sends. "
                                  f"Flight: ./COATHEAL-GroundStation.sh --link-cap {port}")
