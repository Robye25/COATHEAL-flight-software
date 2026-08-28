"""Every file the ground station writes about a flight (redesign spec §7).

Schema v6 is the single telemetry CSV layout shared by the GUI receiver and
the CLI telemetry server (they used to write two different layouts to the
same default file). One session directory holds:

    telemetry.csv   one row per accepted DATA frame (schema v6)
    pulls.csv       one row per EVT,PULL
    commands.csv    every command the operator sent and what came back
    events.log      the event log the operator sees
    session.json    ground-station metadata and counters

`LogManager` owns the current session directory, switches it when the
onboard session id changes, and buffers commands/events that happen before
the first frame so nothing is lost when the directory finally opens.
"""
from __future__ import annotations

import csv
import json
import math
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Deque, Dict, List, Optional, Tuple

from .protocol import PullEvent, TelemetryPacket
from .session_dir import SessionDirectory, session_epoch

TELEMETRY_SCHEMA_VERSION = 6
SAMPLE_COUNT = 8
HEATER_COUNT = 6
MOTOR_COUNT = 2

# (CSV column suffix, key in the `packet.steppers[i]` dict)
STEPPER_COLUMNS = [
    ("position", "position"), ("target", "target"), ("hz", "hz"),
    ("microstep", "microstep"), ("enabled", "enabled"), ("ok", "healthy"),
    ("moving", "moving"), ("holding", "holding"), ("hold_s", "hold_s"),
    ("pulses", "pulses"), ("missed", "missed_deadlines"), ("source", "source"),
    ("zeroed", "zeroed"), ("seq", "seq_name"), ("seqstate", "seq_state"),
]
CTRL_COLUMNS = [
    "fallback", "link_loss_s", "energy_wh", "budget_wh", "budget_exhausted",
    "heaters_active", "queue", "plan",
]


def _build_fields() -> List[str]:
    fields = ["gs_rx_utc", "session_id", "seq", "timestamp", "rtc_valid",
              "ambient_temp_c", "ambient_pressure_mbar", "uv"]
    fields += [f"sample_{i}" for i in range(SAMPLE_COUNT)]
    fields += [f"h{i}" for i in range(HEATER_COUNT)]
    fields += [f"r{i}" for i in range(SAMPLE_COUNT)]
    fields += ["phase", "mode", "status", "sensor_valid", "sensor_age_ms",
               "component_state"]
    for motor in range(MOTOR_COUNT):
        fields += [f"stepper{motor}_{suffix}" for suffix, _ in STEPPER_COLUMNS]
    fields += CTRL_COLUMNS
    return fields


TELEMETRY_CSV_FIELDS: List[str] = _build_fields()
PULL_CSV_FIELDS = ["gs_rx_utc", "session_id", "pull_id", "motor_id", "start_ts",
                   "steps_moved", "hold_s", "samples"]
COMMAND_CSV_FIELDS = ["ts_utc", "command", "ok", "latency_ms", "body", "raw"]

TELEMETRY_FILE = "telemetry.csv"
PULLS_FILE = "pulls.csv"
COMMANDS_FILE = "commands.csv"
EVENTS_FILE = "events.log"
META_FILE = "session.json"


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _num(value: Any) -> str:
    """Lossless, compact CSV rendering: repr for floats, 0/1 for bools,
    'nan'/'inf' spelled out, '' for None."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float):
        if math.isnan(value):
            return "nan"
        if math.isinf(value):
            return "inf" if value > 0 else "-inf"
        return repr(value)
    return str(value)


def _kv(mapping: Dict[str, Any]) -> str:
    return "|".join(f"{key}:{_num(value)}" for key, value in mapping.items())


def packet_to_row(pkt: TelemetryPacket, rx_utc: Optional[str] = None) -> Dict[str, str]:
    """Schema-v6 row for one packet. Every column is always present."""
    row: Dict[str, str] = {
        "gs_rx_utc": rx_utc or utc_now_iso(),
        "session_id": pkt.session_id,
        "seq": str(pkt.seq),
        "timestamp": pkt.timestamp,
        "rtc_valid": _num(int(pkt.rtc_valid)),
        "ambient_temp_c": _num(pkt.ambient_temp_c),
        "ambient_pressure_mbar": _num(pkt.ambient_pressure_mbar),
        "uv": _num(pkt.uv),
        "phase": pkt.phase,
        "mode": pkt.mode,
        "status": pkt.status,
        "sensor_valid": _kv({k: int(v) for k, v in pkt.sensor_valid.items()}),
        "sensor_age_ms": _kv(pkt.sensor_age_ms),
        "component_state": "|".join(f"{k}:{v}" for k, v in pkt.component_state.items()),
    }
    for i in range(SAMPLE_COUNT):
        row[f"sample_{i}"] = _num(pkt.sample_temps_c[i]) if i < len(pkt.sample_temps_c) else ""
    for i in range(HEATER_COUNT):
        row[f"h{i}"] = _num(pkt.heater_duty[i]) if i < len(pkt.heater_duty) else ""
    for i in range(SAMPLE_COUNT):
        value = pkt.sample_resistance_ohm[i] if i < len(pkt.sample_resistance_ohm) else None
        row[f"r{i}"] = _num(value)
    by_motor = {int(s.get("motor_id", idx)): s for idx, s in enumerate(pkt.steppers)}
    for motor in range(MOTOR_COUNT):
        snap = by_motor.get(motor)
        for suffix, key in STEPPER_COLUMNS:
            row[f"stepper{motor}_{suffix}"] = _num(snap.get(key)) if snap is not None else ""
    for column in CTRL_COLUMNS:
        row[column] = pkt.ctrl.get(column, "")
    return row


def pull_to_row(ev: PullEvent, rx_utc: Optional[str] = None) -> Dict[str, str]:
    return {
        "gs_rx_utc": rx_utc or utc_now_iso(),
        "session_id": ev.session_id,
        "pull_id": str(ev.pull_id),
        "motor_id": str(ev.motor_id),
        "start_ts": ev.start_ts,
        "steps_moved": str(ev.steps_moved),
        "hold_s": _num(float(ev.hold_s)),
        "samples": "|".join(str(s) for s in ev.samples),
    }


# ── file appenders ──────────────────────────────────────────────────────────
class CsvAppender:
    """Append-only CSV with a fixed header. If the file on disk carries a
    different header (an older schema), it is left alone and a numbered
    sibling (`telemetry.1.csv`, ...) receives the new rows: nothing is ever
    overwritten and no file ever mixes two layouts."""

    def __init__(self, path: Path, fields: List[str]):
        self.fields = list(fields)
        self.path = self._compatible_path(Path(path))
        self._fh = None
        self._writer: Optional[csv.DictWriter] = None
        self.rows_written = 0

    def _compatible_path(self, path: Path) -> Path:
        candidate = path
        n = 0
        expected = ",".join(self.fields)
        while candidate.exists() and candidate.stat().st_size > 0:
            try:
                with candidate.open("r", encoding="utf-8", newline="") as fh:
                    header = fh.readline().rstrip("\r\n")
            except OSError:
                header = ""
            if header == expected:
                return candidate
            n += 1
            candidate = path.with_name(f"{path.stem}.{n}{path.suffix}")
        return candidate

    def write(self, row: Dict[str, Any]) -> None:
        if self._fh is None:
            fresh = not self.path.exists() or self.path.stat().st_size == 0
            self._fh = self.path.open("a", newline="", encoding="utf-8")
            self._writer = csv.DictWriter(self._fh, fieldnames=self.fields,
                                          extrasaction="ignore")
            if fresh:
                self._writer.writeheader()
        assert self._writer is not None
        self._writer.writerow(row)
        self._fh.flush()
        self.rows_written += 1

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._fh.close()
            finally:
                self._fh = None
                self._writer = None


class TextAppender:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._fh = None
        self.lines_written = 0

    def write(self, line: str) -> None:
        if self._fh is None:
            self._fh = self.path.open("a", encoding="utf-8")
        self._fh.write(line.rstrip("\n") + "\n")
        self._fh.flush()
        self.lines_written += 1

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._fh.close()
            finally:
                self._fh = None


def format_event_line(level: str, message: str, ts_utc: Optional[str] = None) -> str:
    return f"{ts_utc or utc_now_iso()} {level.upper():<5} {message}"


# ── one session ──────────────────────────────────────────────────────────────
class SessionLogs:
    """The writers for one session directory. `meta` is refreshed every
    `META_EVERY` telemetry rows and on close."""

    META_EVERY = 300

    def __init__(self, directory: SessionDirectory, gs_info: Optional[Dict[str, Any]] = None):
        self.directory = directory
        self.dir = directory.ensure()
        self.session_id = directory.session_id
        self.telemetry = CsvAppender(self.dir / TELEMETRY_FILE, TELEMETRY_CSV_FIELDS)
        self.pulls = CsvAppender(self.dir / PULLS_FILE, PULL_CSV_FIELDS)
        self.commands = CsvAppender(self.dir / COMMANDS_FILE, COMMAND_CSV_FIELDS)
        self.events = TextAppender(self.dir / EVENTS_FILE)
        self._meta: Dict[str, Any] = {
            "session_id": self.session_id,
            "schema_version": TELEMETRY_SCHEMA_VERSION,
            "ground_station": dict(gs_info or {}),
            "opened_utc": utc_now_iso(),
            "first_frame_utc": None,
            "last_frame_utc": None,
            "first_seq": None,
            "last_seq": None,
            "frames": 0,
            "pulls": 0,
            "commands": 0,
        }
        self._write_meta()

    def _write_meta(self) -> None:
        self._meta["updated_utc"] = utc_now_iso()
        tmp = self.dir / (META_FILE + ".tmp")
        try:
            tmp.write_text(json.dumps(self._meta, indent=2) + "\n", encoding="utf-8")
            tmp.replace(self.dir / META_FILE)
        except OSError:
            pass

    def write_packet(self, pkt: TelemetryPacket, rx_utc: Optional[str] = None) -> None:
        rx = rx_utc or utc_now_iso()
        self.telemetry.write(packet_to_row(pkt, rx))
        meta = self._meta
        meta["frames"] += 1
        meta["last_frame_utc"] = rx
        meta["last_seq"] = pkt.seq
        if meta["first_frame_utc"] is None:
            meta["first_frame_utc"] = rx
            meta["first_seq"] = pkt.seq
            # Written immediately: a process killed before its first
            # periodic refresh must still leave a truthful session.json.
            self._write_meta()
        elif meta["frames"] % self.META_EVERY == 0:
            self._write_meta()

    def write_pull(self, ev: PullEvent, rx_utc: Optional[str] = None) -> None:
        self.pulls.write(pull_to_row(ev, rx_utc))
        self._meta["pulls"] += 1

    def write_command(self, command: str, ok: bool, latency_ms: float,
                      body: str, raw: str, ts_utc: Optional[str] = None) -> None:
        self.commands.write({
            "ts_utc": ts_utc or utc_now_iso(), "command": command,
            "ok": "1" if ok else "0", "latency_ms": f"{latency_ms:.1f}",
            "body": body, "raw": raw,
        })
        self._meta["commands"] += 1

    def write_event(self, level: str, message: str, ts_utc: Optional[str] = None) -> None:
        self.events.write(format_event_line(level, message, ts_utc))

    def close(self) -> None:
        self._meta["closed_utc"] = utc_now_iso()
        self._write_meta()
        for writer in (self.telemetry, self.pulls, self.commands, self.events):
            writer.close()


# ── manager ──────────────────────────────────────────────────────────────────
class LogManager:
    """Routes every ground-station record to the right session directory.

    Thread-safe: the receiver thread writes telemetry and pulls while the
    GUI thread writes commands and events.
    """

    def __init__(self, root: Path, *, gs_info: Optional[Dict[str, Any]] = None,
                 buffer_limit: int = 20_000):
        self.root = Path(root)
        self._gs_info = dict(gs_info or {})
        self._lock = threading.RLock()
        self._logs: Optional[SessionLogs] = None      # the live (newest) session
        self._open: Dict[str, SessionLogs] = {}        # every session with files open
        self.last_opened: Optional[Tuple[str, Path]] = None
        self._pending_commands: Deque[Dict[str, Any]] = deque(maxlen=buffer_limit)
        self._pending_events: Deque[Dict[str, Any]] = deque(maxlen=buffer_limit)
        self._started = time.time()
        self._closed = False

    # -- state ---------------------------------------------------------------
    @property
    def current_dir(self) -> Optional[Path]:
        with self._lock:
            return self._logs.dir if self._logs is not None else None

    @property
    def current_session_id(self) -> Optional[str]:
        with self._lock:
            return self._logs.session_id if self._logs is not None else None

    @property
    def frames(self) -> int:
        with self._lock:
            return self._logs._meta["frames"] if self._logs is not None else 0

    # -- routing -------------------------------------------------------------
    def _logs_for(self, session_id: str) -> Tuple[SessionLogs, bool]:
        """Session logs for `session_id`, opening them if needed. Returns
        (logs, opened). The live-first drain interleaves the previous
        session's backlog with the current session's live frames, so more
        than one session can be open; commands and events go to the newest
        one (by boot epoch), which is the one the operator is talking to."""
        logs = self._open.get(session_id)
        if logs is not None:
            return logs, False
        logs = SessionLogs(SessionDirectory(self.root, session_id), self._gs_info)
        self._open[session_id] = logs
        self.last_opened = (session_id, logs.dir)
        first = self._logs is None
        if first or (session_epoch(session_id) or 0) >= (session_epoch(self._logs.session_id) or 0):
            self._logs = logs
        if first:
            self._flush_pending(logs)
        # Anything older than the previous session is finished: close it.
        while len(self._open) > 2:
            oldest = min(self._open, key=lambda sid: (session_epoch(sid) or 0, sid))
            if oldest == self._logs.session_id:
                break
            self._open.pop(oldest).close()
        return logs, True

    def dir_for(self, session_id: str) -> Optional[Path]:
        with self._lock:
            logs = self._open.get(session_id)
            return logs.dir if logs is not None else None

    def _flush_pending(self, logs: SessionLogs) -> None:
        while self._pending_events:
            item = self._pending_events.popleft()
            logs.write_event(item["level"], item["message"], item["ts_utc"])
        while self._pending_commands:
            item = self._pending_commands.popleft()
            logs.write_command(**item)

    def on_packet(self, pkt: TelemetryPacket, rx_utc: Optional[str] = None) -> bool:
        """Write the frame. Returns True when this frame opened a (new)
        session directory -- callers log that as an event."""
        with self._lock:
            if self._closed:
                return False
            logs, opened = self._logs_for(pkt.session_id)
            logs.write_packet(pkt, rx_utc)
            return opened

    def on_pull(self, ev: PullEvent, rx_utc: Optional[str] = None) -> None:
        with self._lock:
            if self._closed:
                return
            logs, _opened = self._logs_for(ev.session_id)
            logs.write_pull(ev, rx_utc)

    def log_command(self, command: str, ok: bool, latency_ms: float,
                    body: str = "", raw: str = "", ts_utc: Optional[str] = None) -> None:
        record = {"command": command, "ok": bool(ok), "latency_ms": float(latency_ms),
                  "body": body, "raw": raw, "ts_utc": ts_utc or utc_now_iso()}
        with self._lock:
            if self._closed:
                return
            if self._logs is None:
                self._pending_commands.append(record)
            else:
                self._logs.write_command(**record)

    def log_event(self, level: str, message: str, ts_utc: Optional[str] = None) -> None:
        record = {"level": level, "message": message, "ts_utc": ts_utc or utc_now_iso()}
        with self._lock:
            if self._closed:
                return
            if self._logs is None:
                self._pending_events.append(record)
            else:
                self._logs.write_event(**record)

    def close(self) -> None:
        with self._lock:
            for sid, logs in list(self._open.items()):
                if logs is not self._logs:
                    logs.close()
                    self._open.pop(sid, None)
        with self._lock:
            if self._closed:
                return
            self._closed = True
            if self._logs is None and (self._pending_commands or self._pending_events):
                # Never heard an onboard session: the operator's own record
                # still gets a directory.
                logs = SessionLogs(SessionDirectory.no_session(self.root, now=self._started),
                                   self._gs_info)
                self._flush_pending(logs)
                logs.close()
            elif self._logs is not None:
                self._logs.close()
                self._logs = None
