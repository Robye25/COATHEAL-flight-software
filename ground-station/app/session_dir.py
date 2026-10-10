"""Per-session log directories (redesign spec §7).

One directory per *onboard* session id under ``<root>/sessions/``. The name
starts with the onboard boot time -- parsed from the session id, which the
onboard builds as ``coatheal-<epoch>-<pid>`` -- so directories sort
chronologically, followed by the id itself so the mapping is unambiguous.
A ground-station restart while the onboard session is unchanged resolves to
the same directory and appends to it.
"""
from __future__ import annotations

import re
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

SESSIONS_SUBDIR = "sessions"
LATEST_POINTER = "latest_session.txt"
NO_SESSION_TAG = "no-session"

_SESSION_ID_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*-(?P<epoch>\d{9,11})(?:-\d+)?$")


def session_epoch(session_id: str) -> Optional[int]:
    """Boot epoch embedded in an onboard session id, or None."""
    match = _SESSION_ID_RE.match(session_id.strip())
    return int(match.group("epoch")) if match else None


def safe_component(text: str) -> str:
    """Filesystem-safe rendering of an arbitrary id (never empty)."""
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", text.strip())
    return cleaned[:80] or "unknown"


def _stamp(epoch: float) -> str:
    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")


def session_dir_name(session_id: str, now: Optional[float] = None, *, suffix: str = "") -> str:
    """``<YYYYMMDD-HHMMSS>_<session id>`` -- time from the id when it carries
    one, otherwise from `now` (or the wall clock). With a `suffix` (a log
    session the operator started by hand) the time is `now` and the name
    ends in ``_<suffix>``, so it never collides with the automatic one."""
    epoch: float
    parsed = session_epoch(session_id)
    if parsed is not None and not suffix:
        epoch = float(parsed)
    else:
        epoch = float(now) if now is not None else time.time()
    name = f"{_stamp(epoch)}_{safe_component(session_id)}"
    return f"{name}_{safe_component(suffix)}" if suffix else name


def session_display(session_id: str) -> str:
    """The session for the operator: its boot date and time (from the id)
    and the id without the `coatheal-` prefix, e.g.
    ``2026-10-10 12:20:56Z · 1787760547-1``; an id without an epoch as is."""
    session_id = (session_id or "").strip()
    if not session_id:
        return "—"
    epoch = session_epoch(session_id)
    if epoch is None:
        return session_id
    stamp = datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    tail = session_id.split("-", 1)[1] if "-" in session_id else session_id
    return f"{stamp} · {tail}"


class SessionDirectory:
    """Location of one session's files. Creating it is explicit (`ensure`)."""

    def __init__(self, root: Path, session_id: str, *, now: Optional[float] = None, suffix: str = ""):
        self.root = Path(root)
        self.session_id = session_id
        self.path = self.root / SESSIONS_SUBDIR / session_dir_name(session_id, now, suffix=suffix)

    @classmethod
    def no_session(cls, root: Path, *, now: Optional[float] = None) -> "SessionDirectory":
        """Directory for a ground-station run that never heard an onboard
        session (its command/event logs still deserve a home)."""
        instance = cls.__new__(cls)
        instance.root = Path(root)
        instance.session_id = ""
        epoch = float(now) if now is not None else time.time()
        instance.path = instance.root / SESSIONS_SUBDIR / f"{_stamp(epoch)}_{NO_SESSION_TAG}"
        return instance

    def exists(self) -> bool:
        return self.path.is_dir()

    def ensure(self) -> Path:
        self.path.mkdir(parents=True, exist_ok=True)
        try:
            (self.root / LATEST_POINTER).write_text(str(self.path) + "\n", encoding="utf-8")
        except OSError:
            pass
        return self.path

    def file(self, name: str) -> Path:
        return self.path / name
