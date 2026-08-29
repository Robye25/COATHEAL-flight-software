"""Background I/O: telemetry receiver QThread + async command dispatcher.

The receiver owns the TCP telemetry server (accept, parse, ACK, dedupe) and
hands every accepted frame to the shared `LogManager`, which decides the
session directory and writes the schema-v6 CSV. The dispatcher owns the
one-shot TCP command client, the command history, the command log, and the
radio-silence gate (redesign spec §9): while silent only the whitelist can
leave the ground station.
"""
from __future__ import annotations

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
    timeout_for,
)
from ..reply_format import parse_kv_body
from ..seqset import SeqSet
from ..telemetry_log import LogManager, utc_now_iso

DEFAULT_COMMAND_HOST = "169.254.10.10"

# Commands the ground station is still allowed to send while the onboard is
# in radio silence -- exactly the set the onboard answers (spec §9). Anything
# else is refused locally with `SILENCE_BLOCK_ERROR` before it touches the
# network, so a mis-click cannot break the silence.
SILENCE_WHITELIST = frozenset({"RADIO_RESUME", "RADIO_SILENCE", "STATUS", "PING"})
SILENCE_BLOCK_ERROR = "blocked: radio silence active (only RADIO_RESUME, STATUS, PING are sent)"


def command_verb(command: str) -> str:
    stripped = command.strip()
    return stripped.split()[0].upper() if stripped else ""


# ── telemetry receiver ────────────────────────────────────────────────────────
class TelemetryReceiver(QThread):
    """TCP server listening for onboard telemetry frames.

    Emits `packet_received(TelemetryPacket)` on every fresh frame, routes it
    to the `LogManager`, and de-dupes across reconnects via a cursor JSON.
    """

    packet_received    = pyqtSignal(object)
    pull_event         = pyqtSignal(object)  # PullEvent
    log_message        = pyqtSignal(str)
    connection_changed = pyqtSignal(bool, str)
    status_changed     = pyqtSignal(str)  # "listening" | "connected" | "stale" | "searching" | "failed"
    session_opened     = pyqtSignal(str, str)  # (session_id, directory path)

    _STALE_EMIT_S   = 5.0  # emit "stale" status when DATA frames older than this
    _DATA_TIMEOUT_S = 8.0  # idle window before we force-close the onboard socket
    # The ACK cursor used to be rewritten on every frame (5 writes/s at the
    # top tick rate); once a second is plenty for a crash-recovery hint.
    _CURSOR_MIN_INTERVAL_S = 1.0

    def __init__(self, bind: str, port: int, log_manager: LogManager, parent=None):
        super().__init__(parent)
        self.parse_errors = 0  # malformed frames/events since start (status bar)
        self._bind = bind
        self._port = port
        self._logs = log_manager
        self._stop_flag = threading.Event()
        # Received (session -> SeqSet). Frames arrive out of order (the
        # onboard sends this tick's frame before its backlog), so "seq <=
        # last seen" would drop a whole backlog as duplicates.
        self._received_by_session: dict[str, SeqSet] = {}
        self._cursor_dirty = False
        self._cursor_last_persist = 0.0
        self._load_cursor()

    @property
    def log_manager(self) -> LogManager:
        return self._logs

    def _cursor_path(self) -> Path:
        return self._logs.root / "ground_ack_cursor.json"

    def _load_cursor(self) -> None:
        p = self._cursor_path()
        if not p.exists():
            return
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            self._received_by_session = {
                str(k): SeqSet.from_json(v) for k, v in data.get("sessions", {}).items()
            }
        except Exception:
            pass

    def _persist_cursor(self, force: bool = False) -> None:
        if not self._cursor_dirty:
            return
        now = time.monotonic()
        if not force and (now - self._cursor_last_persist) < self._CURSOR_MIN_INTERVAL_S:
            return
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "sessions": {sid: seen.to_json() for sid, seen in self._received_by_session.items()},
        }
        try:
            self._cursor_path().write_text(json.dumps(payload, indent=2), encoding="utf-8")
            self._cursor_dirty = False
            self._cursor_last_persist = now
        except OSError:
            pass

    def stop(self) -> None:
        self._stop_flag.set()

    def run(self) -> None:
        try:
            self._logs.root.mkdir(parents=True, exist_ok=True)
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
                        self._handle_connection(conn)
                    except OSError as exc:
                        self.log_message.emit(f"[telemetry] connection reset: {exc}")
                    finally:
                        conn.close()
                        self._persist_cursor(force=True)
                        self.connection_changed.emit(False, "")
                        self.status_changed.emit("searching")
                        self.log_message.emit("[telemetry] onboard disconnected")
        except Exception as exc:
            self.log_message.emit(f"[error] receiver fatal: {exc}")
            # Distinct from "searching"/"stale". This `except` wraps the
            # WHOLE run() body -- the bind, the accept loop, and every
            # connection handled -- so "failed" does not mean "the bind
            # never happened" specifically; it can just as easily fire
            # mid-run after one or more successful connections. The log
            # line above carries the real cause; MainWindow's status label
            # stays cause-neutral and points there.
            self.status_changed.emit("failed")
        finally:
            self._persist_cursor(force=True)
            self._stop_flag.set()

    def _handle_connection(self, conn: socket.socket) -> None:
        conn.settimeout(1.0)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        buf = ""
        last_data = time.monotonic()
        stale_emitted = False

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
                self._persist_cursor()
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
                rx_utc = utc_now_iso()
                # Route PULL events to their own signal + ACK them
                # cumulatively (seq=0). Same framing as EVT,CYCLE so
                # the onboard queue clears in-order.
                if line.startswith("EVT,PULL,"):
                    try:
                        ev = parse_pull_event(line)
                    except TelemetryParseError as exc:
                        self.parse_errors += 1
                        self.log_message.emit(f"[parse-error] {exc}")
                        continue
                    try:
                        conn.sendall(build_ack(ev.session_id, 0).encode("utf-8"))
                    except OSError:
                        return
                    self._logs.on_pull(ev, rx_utc)
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
                    self.parse_errors += 1
                    self.log_message.emit(f"[parse-error] {exc}")
                    continue

                seen = self._received_by_session.get(pkt.session_id)
                if seen is None:
                    seen = self._received_by_session[pkt.session_id] = SeqSet()
                is_dup = not seen.add(pkt.seq)
                if not is_dup:
                    self._cursor_dirty = True
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

                if self._logs.on_packet(pkt, rx_utc):
                    directory = self._logs.dir_for(pkt.session_id) or self._logs.current_dir
                    self.log_message.emit(
                        f"[log] session {pkt.session_id} -> {directory}")
                    self.session_opened.emit(pkt.session_id, str(directory))

                self.packet_received.emit(pkt)


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
    fires back on the Qt event thread. Every response -- including the
    local refusals of the radio-silence gate -- lands in the history and
    in the session's `commands.csv`.
    """

    response_received = pyqtSignal(str, object, float, object)  # cmd, CommandResponse, ms, tag
    # Replies to `quiet` sends (high-rate polls such as the Debug tab's
    # MOTOR_DEBUG): delivered here only -- not to the console, the history
    # or commands.csv -- so a 2 Hz probe does not bury the operator's record.
    quiet_response = pyqtSignal(str, object, float, object)
    silence_changed = pyqtSignal(bool)

    def __init__(self, host: str, port: int, history_size: int = 200,
                 log_manager: Optional[LogManager] = None):
        super().__init__()
        self.host = self._normalize_host(host)
        self.port = port
        self._pool = QThreadPool.globalInstance()
        self._history: Deque[CommandHistoryEntry] = deque(maxlen=history_size)
        self._log_manager = log_manager
        self._silence = False
        self.response_received.connect(self._on_response)

    @staticmethod
    def _normalize_host(host: str) -> str:
        value = (host or "").strip()
        return value or DEFAULT_COMMAND_HOST

    def set_endpoint(self, host: str, port: int) -> None:
        self.host = self._normalize_host(host)
        self.port = port

    def set_log_manager(self, log_manager: Optional[LogManager]) -> None:
        self._log_manager = log_manager

    # -- radio silence gate --------------------------------------------------
    @property
    def silence(self) -> bool:
        return self._silence

    def set_silence(self, active: bool) -> None:
        active = bool(active)
        if active == self._silence:
            return
        self._silence = active
        self.silence_changed.emit(active)

    def blocked_reason(self, command: str) -> Optional[str]:
        """Why `command` would not be sent right now, or None."""
        if self._silence and command_verb(command) not in SILENCE_WHITELIST:
            return SILENCE_BLOCK_ERROR
        return None

    def send(self, command: str, tag: Optional[object] = None,
             timeout: Optional[float] = None, quiet: bool = False) -> None:
        emit = self.quiet_response.emit if quiet else self.response_received.emit
        reason = self.blocked_reason(command)
        if reason is not None:
            resp = CommandResponse(ok=False, command=command.strip(), error=reason, raw="")
            # Delivered synchronously: a refusal is not a network event and
            # every consumer (history, log, response line) must see it in
            # the same order as the click that caused it.
            emit(command, resp, 0.0, tag)
            return
        # `timeout=None` (the default) resolves per-verb via
        # protocol.timeout_for -- CHECK gets a longer budget than the plain
        # 3.0s default (see protocol.COMMAND_TIMEOUTS for why). An
        # explicitly-passed timeout always wins over the table.
        resolved_timeout = timeout if timeout is not None else timeout_for(command)
        job = _SendJob(self.host, self.port, command, resolved_timeout, tag, emit)
        self._pool.start(job)

    def _on_response(self, cmd: str, resp: CommandResponse, ms: float, _tag) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        self._history.append(CommandHistoryEntry(ts=ts, command=cmd, ok=resp.ok,
                                                 latency_ms=ms, response=resp))
        if self._log_manager is not None:
            body = resp.body if resp.ok else (resp.error or resp.raw)
            self._log_manager.log_command(cmd, resp.ok, ms, body, resp.raw)
        self._track_silence(cmd, resp)

    def _track_silence(self, cmd: str, resp: CommandResponse) -> None:
        if not resp.ok:
            return
        verb = command_verb(cmd)
        if verb == "RADIO_SILENCE":
            self.set_silence(True)
        elif verb == "RADIO_RESUME":
            self.set_silence(False)
        elif verb == "STATUS":
            flag = parse_kv_body(resp.body).get("silence")
            if flag in ("0", "1"):
                self.set_silence(flag == "1")

    def history(self) -> list[CommandHistoryEntry]:
        return list(self._history)
