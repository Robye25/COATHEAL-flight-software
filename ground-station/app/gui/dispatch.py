"""Background I/O: telemetry receiver QThread + async command dispatcher.

Split out of the original gui_app.py so panels can be tested in isolation.
"""
from __future__ import annotations

import csv
import json
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Deque, Optional

from PyQt6.QtCore import QObject, QRunnable, QThread, QThreadPool, pyqtSignal

from ..protocol import (
    CommandResponse,
    PullEvent,
    TelemetryPacket,
    TelemetryParseError,
    build_ack,
    parse_command_response,
    parse_pull_event,
    parse_telemetry_csv,
)


# ── telemetry receiver ────────────────────────────────────────────────────────
class TelemetryReceiver(QThread):
    """TCP server listening for onboard telemetry frames.

    Emits `packet_received(TelemetryPacket)` on every fresh frame, writes to a
    CSV mirror, and de-dupes across reconnects via a cursor JSON.
    """

    packet_received    = pyqtSignal(object)
    pull_event         = pyqtSignal(object)  # PullEvent
    log_message        = pyqtSignal(str)
    connection_changed = pyqtSignal(bool, str)
    status_changed     = pyqtSignal(str)  # "listening" | "connected" | "stale" | "searching"

    # CSV v3 — v2 (single stepper) columns remain at their fixed positions
    # so existing post-flight parsers stay valid, and a second block of
    # `m1_*` columns is appended for the Rev-B second motor. When a frame
    # only carries one motor (legacy DATA or the new DATA if M1 was
    # omitted), the `m1_*` cells are written empty.
    CSV_FIELDS = [
        "session_id", "seq", "timestamp", "rtc_valid",
        "ambient_temp_c", "ambient_pressure_mbar", "ambient_humidity_pct",
        "uv", "box_temp_c", "sample_temps_c", "heater_duty", "phase", "status",
        "mode",
        # Motor 0 (legacy column names kept).
        "stepper_pos", "stepper_tgt", "stepper_hz", "stepper_us",
        "stepper_en", "stepper_mv", "stepper_hold", "stepper_hold_s",
        "stepper_pulses", "stepper_src",
        # Motor 1 (Rev-B addition).
        "m1_pos", "m1_tgt", "m1_hz", "m1_us",
        "m1_en", "m1_mv", "m1_hold", "m1_hold_s",
        "m1_pulses", "m1_src",
    ]

    _STALE_EMIT_S   = 5.0  # emit "stale" status when DATA frames older than this
    _DATA_TIMEOUT_S = 8.0  # idle window before we force-close the onboard socket

    def __init__(self, bind: str, port: int, log_path: Path, parent=None):
        super().__init__(parent)
        self._bind = bind
        self._port = port
        self._log_path = log_path
        self._stop_flag = threading.Event()
        self._last_seq_by_session: dict[str, int] = {}
        self._load_cursor()

    def _cursor_path(self) -> Path:
        return self._log_path.parent / "ground_ack_cursor.json"

    def _load_cursor(self) -> None:
        p = self._cursor_path()
        if not p.exists():
            return
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            self._last_seq_by_session = {
                str(k): int(v) for k, v in data.get("sessions", {}).items()
            }
        except Exception:
            pass

    def _persist_cursor(self) -> None:
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "sessions": self._last_seq_by_session,
        }
        try:
            self._cursor_path().write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except OSError:
            pass

    def stop(self) -> None:
        self._stop_flag.set()

    def run(self) -> None:
        self._log_path.parent.mkdir(parents=True, exist_ok=True)
        first_write = not self._log_path.exists()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind((self._bind, self._port))
                srv.listen(5)
                srv.settimeout(1.0)
                self.log_message.emit(
                    f"[telemetry] listening on {self._bind}:{self._port}"
                )
                self.status_changed.emit("listening")
                while not self._stop_flag.is_set():
                    try:
                        conn, addr = srv.accept()
                    except socket.timeout:
                        continue
                    addr_str = f"{addr[0]}:{addr[1]}"
                    self.connection_changed.emit(True, addr_str)
                    self.status_changed.emit("connected")
                    self.log_message.emit(f"[telemetry] onboard connected from {addr_str}")
                    try:
                        self._handle_connection(conn, first_write)
                        first_write = False
                    except OSError as exc:
                        self.log_message.emit(f"[telemetry] connection reset: {exc}")
                    finally:
                        conn.close()
                        self.connection_changed.emit(False, "")
                        self.status_changed.emit("searching")
                        self.log_message.emit("[telemetry] onboard disconnected")
        except Exception as exc:
            self.log_message.emit(f"[error] receiver fatal: {exc}")
        finally:
            self._stop_flag.set()

    def _handle_connection(self, conn: socket.socket, write_header: bool) -> None:
        conn.settimeout(1.0)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        buf = ""
        last_data = time.monotonic()
        stale_emitted = False
        with self._log_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=self.CSV_FIELDS)
            if write_header:
                writer.writeheader()

            while not self._stop_flag.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    idle = time.monotonic() - last_data
                    if idle > self._DATA_TIMEOUT_S:
                        self.log_message.emit(
                            f"[telemetry] no data for {self._DATA_TIMEOUT_S:.0f}s "
                            "— closing stale connection, waiting for reconnect"
                        )
                        return
                    if idle > self._STALE_EMIT_S and not stale_emitted:
                        stale_emitted = True
                        self.status_changed.emit("stale")
                    continue
                except OSError:
                    return
                if not chunk:
                    break
                last_data = time.monotonic()
                if stale_emitted:
                    stale_emitted = False
                    self.status_changed.emit("connected")

                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    # Route PULL events to their own signal + ACK them
                    # cumulatively (seq=0). Same framing as EVT,CYCLE so
                    # the onboard queue clears in-order.
                    if line.startswith("EVT,PULL,"):
                        try:
                            ev = parse_pull_event(line)
                        except TelemetryParseError as exc:
                            self.log_message.emit(f"[parse-error] {exc}")
                            continue
                        try:
                            conn.sendall(build_ack(ev.session_id, 0).encode("utf-8"))
                        except OSError:
                            return
                        self.pull_event.emit(ev)
                        self.log_message.emit(
                            f"[evt][pull] motor={ev.motor_id} pull_id={ev.pull_id} "
                            f"steps={ev.steps_moved} hold={ev.hold_s:.1f}s "
                            f"samples={'|'.join(str(s) for s in ev.samples) or '-'}"
                        )
                        continue
                    try:
                        pkt = parse_telemetry_csv(line)
                    except TelemetryParseError as exc:
                        self.log_message.emit(f"[parse-error] {exc}")
                        continue

                    last_seq = self._last_seq_by_session.get(pkt.session_id, -1)
                    is_dup = pkt.seq <= last_seq
                    if not is_dup:
                        self._last_seq_by_session[pkt.session_id] = pkt.seq
                        self._persist_cursor()

                    try:
                        conn.sendall(build_ack(pkt.session_id, pkt.seq).encode("utf-8"))
                    except OSError:
                        return

                    if is_dup:
                        self.log_message.emit(
                            f"[dup] dropped seq={pkt.seq} session={pkt.session_id}"
                        )
                        continue

                    writer.writerow(_packet_to_csv_row(pkt))
                    f.flush()

                    self.packet_received.emit(pkt)


def _motor_cells(m: Optional[dict], prefix: str) -> dict:
    """Flatten a stepper snapshot dict into CSV cells.

    Returns an empty-value dict when the motor is missing so the CSV row
    remains the full width.
    """
    keys = ["pos", "tgt", "hz", "us", "en", "mv", "hold", "hold_s",
            "pulses", "src"]
    if m is None:
        return {f"{prefix}_{k}": "" for k in keys}
    return {
        f"{prefix}_pos":    m["position"],
        f"{prefix}_tgt":    m["target"],
        f"{prefix}_hz":     m["hz"],
        f"{prefix}_us":     m["microstep"],
        f"{prefix}_en":     int(m["enabled"]),
        f"{prefix}_mv":     int(m["moving"]),
        f"{prefix}_hold":   int(m["holding"]),
        f"{prefix}_hold_s": m["hold_s"],
        f"{prefix}_pulses": m["pulses"],
        f"{prefix}_src":    m["source"],
    }


def _packet_to_csv_row(pkt: TelemetryPacket) -> dict:
    # Motor 0 keeps the legacy `stepper_*` prefix for back-compat; motor 1
    # uses the new `m1_*` prefix.
    m0 = pkt.steppers[0] if pkt.steppers else None
    m1 = pkt.steppers[1] if len(pkt.steppers) > 1 else None
    row = {
        "session_id": pkt.session_id,
        "seq": pkt.seq,
        "timestamp": pkt.timestamp,
        "rtc_valid": pkt.rtc_valid,
        "ambient_temp_c": pkt.ambient_temp_c,
        "ambient_pressure_mbar": pkt.ambient_pressure_mbar,
        "ambient_humidity_pct": pkt.ambient_humidity_pct,
        "uv": pkt.uv,
        "box_temp_c": pkt.box_temp_c,
        "sample_temps_c": "|".join(f"{x:.2f}" for x in pkt.sample_temps_c),
        "heater_duty":    "|".join(f"{x:.3f}" for x in pkt.heater_duty),
        "phase": pkt.phase,
        "status": pkt.status,
        "mode": pkt.mode,
    }
    row.update(_motor_cells(m0, "stepper"))
    row.update(_motor_cells(m1, "m1"))
    return row


# ── async command dispatcher ─────────────────────────────────────────────────
@dataclass
class CommandHistoryEntry:
    ts: str              # local time, e.g. "14:23:07"
    command: str
    ok: bool
    latency_ms: float
    response: CommandResponse


class _SendJob(QRunnable):
    def __init__(self, host: str, port: int, command: str, timeout: float,
                 tag: Optional[object], signal_emit: Callable[[str, CommandResponse, float, object], None]):
        super().__init__()
        self._host = host
        self._port = port
        self._cmd = command
        self._timeout = timeout
        self._tag = tag
        self._emit = signal_emit

    def run(self) -> None:
        start = time.monotonic()
        payload = (self._cmd.strip() + "\n").encode("utf-8")
        try:
            with socket.create_connection((self._host, self._port), timeout=self._timeout) as s:
                s.sendall(payload)
                data = s.recv(4096)
            raw = data.decode("utf-8", errors="replace").strip()
            resp = parse_command_response(raw) if raw else CommandResponse(
                ok=False, command=self._cmd, error="empty reply", raw="")
        except Exception as exc:
            resp = CommandResponse(ok=False, command=self._cmd, error=str(exc), raw="")
        latency_ms = (time.monotonic() - start) * 1000.0
        self._emit(self._cmd, resp, latency_ms, self._tag)


class CommandDispatcher(QObject):
    """Fire-and-forget command client. Callers don't block the GUI thread.

    `send(cmd, tag=widget)` queues a send; `response_received(cmd, resp, ms, tag)`
    fires back on the Qt event thread so toasts can anchor to the button that
    originated the command.
    """

    response_received = pyqtSignal(str, object, float, object)  # cmd, CommandResponse, ms, tag

    def __init__(self, host: str, port: int, history_size: int = 200):
        super().__init__()
        self.host = host
        self.port = port
        self._pool = QThreadPool.globalInstance()
        self._history: Deque[CommandHistoryEntry] = deque(maxlen=history_size)
        self.response_received.connect(self._on_response)

    def set_endpoint(self, host: str, port: int) -> None:
        self.host = host
        self.port = port

    def send(self, command: str, tag: Optional[object] = None, timeout: float = 3.0) -> None:
        job = _SendJob(self.host, self.port, command, timeout, tag, self.response_received.emit)
        self._pool.start(job)

    def _on_response(self, cmd: str, resp: CommandResponse, ms: float, _tag) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        self._history.append(CommandHistoryEntry(ts=ts, command=cmd, ok=resp.ok,
                                                 latency_ms=ms, response=resp))

    def history(self) -> list[CommandHistoryEntry]:
        return list(self._history)
