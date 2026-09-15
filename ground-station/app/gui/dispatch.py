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

from ..link_budget import (
    TELEMETRY_CLOSE_BYTES,
    LinkBudget,
    Priority,
    budget_wait_error,
    command_budget_wait_s,
    command_exchange_bytes,
    ground_budget,
    paced_connection,
    priority_for,
    request_too_long,
)
from ..link_codec import CodecError, decode_line, describe_hello, hello_reply
from ..protocol import (
    CommandResponse,
    PullEvent,
    TelemetryPacket,
    TelemetryParseError,
    ack_for_raw_line,
    build_ack,
    parse_command_response,
    parse_pull_event,
    parse_telemetry_csv,
    recv_reply_line,
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
CLOSING_ERROR = "not sent: the ground station is closing"


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
    # Idle window before the onboard socket is force-closed. Must exceed the
    # slowest legal tick (SET_TICK_HZ 0.1 = one frame per 10 s) or the
    # receiver would drop a healthy connection between every two frames.
    _DATA_TIMEOUT_S = 12.0
    # The ACK cursor used to be rewritten on every frame (5 writes/s at the
    # top tick rate); once a second is plenty for a crash-recovery hint.
    _CURSOR_MIN_INTERVAL_S = 1.0

    def __init__(self, bind: str, port: int, log_manager: LogManager, parent=None,
                 budget: Optional[LinkBudget] = None):
        super().__init__(parent)
        # Closing a quiet connection is ground-station traffic too.
        self._budget = budget if budget is not None else ground_budget()
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
                    # The FIN and the onboard's ACK of it are charged first;
                    # with no room yet, the quiet connection waits a second.
                    if self._budget.try_charge(TELEMETRY_CLOSE_BYTES, Priority.COMMAND) is None:
                        continue
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
                reply = hello_reply(line)
                if reply is not None:
                    # Codec negotiation (docs/link-budget.md): not a frame,
                    # never ACKed, answered on the same socket.
                    try:
                        conn.sendall(reply.encode("utf-8"))
                    except OSError:
                        return
                    self.log_message.emit(f"[telemetry] {describe_hello(line, reply)}")
                    continue
                if line.startswith("Z1,"):
                    try:
                        line = decode_line(line).strip()
                    except CodecError as exc:
                        # Nothing identifies the frame, so nothing is ACKed.
                        if not self._ack_unparseable(conn, line, exc):
                            return
                        continue
                    if not line:
                        continue
                rx_utc = utc_now_iso()
                # Route PULL events to their own signal + ACK them
                # cumulatively (seq=0). Same framing as EVT,CYCLE so
                # the onboard queue clears in-order.
                if line.startswith("EVT,PULL,"):
                    try:
                        ev = parse_pull_event(line)
                    except Exception as exc:  # parse error of any shape
                        if not self._ack_unparseable(conn, line, exc):
                            return
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
                except Exception as exc:  # TelemetryParseError, or anything else
                    # Never let one bad line take the receiver down: the
                    # thread would die, and the un-ACKed frame would be
                    # re-sent by the onboard on every reconnect.
                    if not self._ack_unparseable(conn, line, exc):
                        return
                    continue

                seen = self._received_by_session.get(pkt.session_id)
                if seen is None:
                    seen = self._received_by_session[pkt.session_id] = SeqSet()
                is_dup = not seen.add(pkt.seq)
                if not is_dup:
                    self._cursor_dirty = True

                # ACK before the cursor write: the onboard resets a link whose
                # ACK misses its 180 ms deadline (docs/link-budget.md).
                try:
                    conn.sendall(build_ack(pkt.session_id, pkt.seq).encode("utf-8"))
                except OSError:
                    return
                if not is_dup:
                    self._persist_cursor()

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

    def _ack_unparseable(self, conn: socket.socket, line: str, exc: Exception) -> bool:
        """Log an unparseable frame, keep its raw text, and ACK whatever
        identity can be read off it so the onboard drops it from its queue.
        Returns False only when the socket is gone."""
        self.parse_errors += 1
        self.log_message.emit(f"[parse-error] {exc}")
        self._logs.log_event("WARN", f"unparseable frame ({exc}): {line}")
        ack = ack_for_raw_line(line)
        if ack is None:
            return True
        try:
            conn.sendall(ack.encode("utf-8"))
        except OSError:
            return False
        return True


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
                 tag: Optional[object], signal_emit: Callable[[str, CommandResponse, float, object], None],
                 priority: Priority = Priority.COMMAND, budget: Optional[LinkBudget] = None,
                 cancel: Optional[threading.Event] = None):
        super().__init__()
        self._host = host
        self._port = port
        self._cmd = command
        self._timeout = timeout
        self._tag = tag
        self._emit = signal_emit
        self._priority = priority
        self._budget = budget if budget is not None else ground_budget()
        self._cancel = cancel

    def run(self) -> None:
        start = time.monotonic()
        resp = self._exchange((self._cmd.strip() + "\n").encode("utf-8"))
        # Includes the wait for the link budget: the operator sees how long
        # the command really took.
        latency_ms = (time.monotonic() - start) * 1000.0
        self._emit(self._cmd, resp, latency_ms, self._tag)

    def _exchange(self, payload: bytes) -> CommandResponse:
        refusal = request_too_long(len(payload))
        if refusal is not None:
            return CommandResponse(ok=False, command=self._cmd, error=refusal, raw="")
        # The whole exchange (SYN to the last FIN) is held on the ground
        # station's 1 150 B share from before connecting until the socket is
        # closed (docs/link-budget.md).
        wait_s = command_budget_wait_s(self._timeout)
        ticket = self._budget.hold(command_exchange_bytes(len(payload)), self._priority, wait_s, self._cancel)
        if ticket is None:
            error = (CLOSING_ERROR if self._cancel is not None and self._cancel.is_set()
                     else budget_wait_error(wait_s))
            return CommandResponse(ok=False, command=self._cmd, error=error, raw="")
        try:
            # Releases the hold once the connection is closed (a failed
            # exchange is reset, so no late reply can follow it).
            with paced_connection(self._budget, ticket, self._host, self._port, self._timeout) as s:
                s.sendall(payload)
                raw = recv_reply_line(s)
            return parse_command_response(raw) if raw else CommandResponse(
                ok=False, command=self._cmd, error="empty reply", raw="")
        except Exception as exc:
            return CommandResponse(ok=False, command=self._cmd, error=str(exc), raw="")


class CommandDispatcher(QObject):
    """Fire-and-forget command client. Callers don't block the GUI thread.

    `send(cmd, tag=widget)` queues a send; `response_received(cmd, resp, ms, tag)`
    fires back on the Qt event thread. Every response -- including the
    local refusals of the radio-silence gate -- lands in the history and
    in the session's `commands.csv`.

    Every exchange waits for room on the ground station's link budget
    (`link_budget.ground_budget()`, shared with discovery), safety commands
    first; at most about one exchange a second fits the 24 kbps share.
    """

    response_received = pyqtSignal(str, object, float, object)  # cmd, CommandResponse, ms, tag
    # Replies to `quiet` sends (high-rate polls such as the Debug tab's
    # MOTOR_DEBUG): delivered here only -- not to the console, the history
    # or commands.csv -- so a 2 Hz probe does not bury the operator's record.
    quiet_response = pyqtSignal(str, object, float, object)
    silence_changed = pyqtSignal(bool)

    def __init__(self, host: str, port: int, history_size: int = 200,
                 log_manager: Optional[LogManager] = None, budget: Optional[LinkBudget] = None):
        super().__init__()
        self.host = self._normalize_host(host)
        self.port = port
        self._pool = QThreadPool.globalInstance()
        self._budget = budget if budget is not None else ground_budget()
        self._history: Deque[CommandHistoryEntry] = deque(maxlen=history_size)
        self._log_manager = log_manager
        self._silence = False
        # Set by close(): commands still waiting for the link budget give up.
        self._closing = threading.Event()
        # (command, tag) of quiet polls still waiting or in flight.
        self._quiet_pending: set[tuple[str, int]] = set()
        self.response_received.connect(self._on_response)
        self.quiet_response.connect(self._on_quiet_response)

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
             timeout: Optional[float] = None, quiet: bool = False) -> bool:
        """Queue `command`. False only for a quiet poll whose previous send
        for the same tag has not come back yet: it is dropped, not queued
        behind it (a 2 Hz poll outruns a link that carries about one
        exchange a second)."""
        emit = self.quiet_response.emit if quiet else self.response_received.emit
        reason = self.blocked_reason(command)
        if reason is not None:
            resp = CommandResponse(ok=False, command=command.strip(), error=reason, raw="")
            # Delivered synchronously: a refusal is not a network event and
            # every consumer (history, log, response line) must see it in
            # the same order as the click that caused it.
            emit(command, resp, 0.0, tag)
            return True
        if quiet:
            key = (command.strip(), id(tag))
            if key in self._quiet_pending:
                return False
            self._quiet_pending.add(key)
        # `timeout=None` (the default) resolves per-verb via
        # protocol.timeout_for -- CHECK gets a longer budget than the plain
        # 3.0s default (see protocol.COMMAND_TIMEOUTS for why). An
        # explicitly-passed timeout always wins over the table.
        resolved_timeout = timeout if timeout is not None else timeout_for(command)
        priority = priority_for(command, quiet)
        job = _SendJob(self.host, self.port, command, resolved_timeout, tag, emit,
                       priority=priority, budget=self._budget, cancel=self._closing)
        # Jobs waiting for a pool thread leave in link-budget priority order,
        # so a safety command never queues behind a batch of ordinary ones.
        self._pool.start(job, int(Priority.DISCOVERY) - int(priority))
        return True

    def _on_quiet_response(self, cmd: str, _resp: CommandResponse, _ms: float, tag) -> None:
        self._quiet_pending.discard((cmd.strip(), id(tag)))

    def close(self) -> None:
        """The window is closing: every command still waiting for the link
        budget fails at once instead of keeping its pool thread (and the
        process) alive for up to its budget wait."""
        self._closing.set()

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
