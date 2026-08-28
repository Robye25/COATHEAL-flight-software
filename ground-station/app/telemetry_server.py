from __future__ import annotations

import argparse
import collections
import math
import socket
import threading
import time
from pathlib import Path
from typing import Optional

from .protocol import (
    HeatingCycleEvent,
    PullEvent,
    TelemetryParseError,
    build_ack,
    parse_heating_cycle_event,
    parse_pull_event,
    parse_telemetry_csv,
)
from .telemetry_log import CsvAppender, LogManager, utc_now_iso

DEFAULT_STATIC_ONBOARD_HOST = "169.254.10.10"
DEFAULT_LOG_ROOT = Path("logs")

CYCLE_CSV_FIELDS = ["gs_rx_utc", "session_id", "cycle_id", "start_ts", "peak_temp_c",
                    "hold_duration_s", "cooldown_rate_c_per_s", "specimen_index", "raw"]


class LivePlotter:
    def __init__(self) -> None:
        import matplotlib.pyplot as plt

        plt.ion()
        self._plt = plt
        # Thread-safe: network thread appends, main thread pops
        self._buf: collections.deque = collections.deque()
        self._seq: list[int] = []
        self._amb_temp: list[float] = []
        self._pressure: list[float] = []

        self._fig, (self._ax_temp, self._ax_pressure) = plt.subplots(2, 1, figsize=(10, 7))

        # No box-temperature trace. Ambient temperature is
        # the only single-value scalar we still plot here.
        (self._temp_line,) = self._ax_temp.plot([], [], label="Ambient Temp [C]")
        self._ax_temp.set_ylabel("Temperature [C]")
        self._ax_temp.grid(True)
        self._ax_temp.legend(loc="best")

        (self._pressure_line,) = self._ax_pressure.plot([], [], label="Ambient Pressure [mbar]")
        self._ax_pressure.set_ylabel("Pressure [mbar]")
        self._ax_pressure.set_xlabel("SEQ")
        self._ax_pressure.grid(True)
        self._ax_pressure.legend(loc="best")

        self._fig.tight_layout()

    def push(self, seq: int, ambient_temp_c: float, pressure_mbar: float) -> None:
        """Called from network thread. deque.append is thread-safe."""
        self._buf.append((seq, ambient_temp_c, pressure_mbar))

    def tick(self) -> None:
        """Called from main thread only. Drains buffer and redraws at up to 20 fps."""
        changed = False
        while self._buf:
            seq, temp, pres = self._buf.popleft()
            self._seq.append(seq)
            self._amb_temp.append(temp)
            self._pressure.append(pres)
            changed = True

        if changed:
            if len(self._seq) > 600:
                self._seq = self._seq[-600:]
                self._amb_temp = self._amb_temp[-600:]
                self._pressure = self._pressure[-600:]

            self._temp_line.set_data(self._seq, self._amb_temp)
            self._pressure_line.set_data(self._seq, self._pressure)

            self._ax_temp.relim()
            self._ax_temp.autoscale_view()
            self._ax_pressure.relim()
            self._ax_pressure.autoscale_view()

            self._fig.canvas.draw_idle()

        # 50 ms pause drives the GUI event loop at ~20 fps; never called from network thread
        self._plt.pause(0.05)


class TelemetryServer:
    """Headless telemetry receiver. Writes exactly the files the GUI writes
    (`telemetry_log.LogManager`, schema v6, one directory per onboard
    session under `<log_root>/sessions/`)."""

    def __init__(
        self,
        bind: str,
        port: int,
        log_root: Path,
        plot: bool,
        alert_temp_c: float,
        timeout_s: float,
        discovery_enabled: bool,
        discovery_port: int,
        command_port: int,
        cursor_path: Path,
        discovered_path: Path,
    ):
        self.bind = bind
        self.port = port
        self.log_root = Path(log_root)
        self.plot = plot
        self.alert_temp_c = alert_temp_c
        self.timeout_s = timeout_s

        self.discovery_enabled = discovery_enabled
        self.discovery_port = discovery_port
        self.command_port = command_port
        self.cursor_path = cursor_path
        self.discovered_path = discovered_path

        self._stop = threading.Event()
        self._last_packet_time = 0.0
        self._last_wait_log_time = 0.0
        self._plotter: Optional[LivePlotter] = None

        self._lock = threading.Lock()
        self._last_seq_by_session: dict[str, int] = {}
        self._seen_pull_ids: set[tuple[str, int]] = set()
        self._last_onboard_ip = ""
        self._last_onboard_session = ""
        self.logs = LogManager(self.log_root, gs_info={
            "component": "telemetry-server", "bind": bind, "port": port,
        })
        self._cycles: Optional[CsvAppender] = None
        self._cycles_dir: Optional[Path] = None

        self._load_cursor()

    def run(self) -> None:
        self.log_root.mkdir(parents=True, exist_ok=True)
        self.cursor_path.parent.mkdir(parents=True, exist_ok=True)
        self.discovered_path.parent.mkdir(parents=True, exist_ok=True)

        if self.plot:
            try:
                self._plotter = LivePlotter()
                print("[telemetry] live plotting enabled")
            except Exception as exc:  # pylint: disable=broad-except
                print(f"[telemetry] plotting disabled: {exc}")
                self._plotter = None

        discovery_thread: Optional[threading.Thread] = None
        if self.discovery_enabled:
            discovery_thread = threading.Thread(target=self._discovery_loop, daemon=True)
            discovery_thread.start()
            print(f"[discovery] enabled on UDP {self.discovery_port}")

        # Network I/O runs on a background thread so matplotlib stays on the main thread.
        net_thread = threading.Thread(target=self._network_loop, daemon=True)
        net_thread.start()

        try:
            if self._plotter is not None:
                # Main thread drives the GUI at ~20 fps until stopped.
                while not self._stop.is_set():
                    self._plotter.tick()
            else:
                net_thread.join()
        except KeyboardInterrupt:
            print("\n[telemetry] stopping")
        finally:
            self._stop.set()
            net_thread.join(timeout=3.0)
            if discovery_thread is not None:
                discovery_thread.join(timeout=2.0)
            self.logs.close()

    def stop(self) -> None:
        self._stop.set()

    def _load_cursor(self) -> None:
        if not self.cursor_path.exists():
            return

        try:
            import json
            data = json.loads(self.cursor_path.read_text(encoding="utf-8"))
            sessions = data.get("sessions", {})
            if isinstance(sessions, dict):
                parsed: dict[str, int] = {}
                for session_id, seq in sessions.items():
                    try:
                        parsed[str(session_id)] = int(seq)
                    except (TypeError, ValueError):
                        continue
                self._last_seq_by_session = parsed
        except (OSError, ValueError):
            self._last_seq_by_session = {}

    def _persist_cursor(self) -> None:
        import json
        from datetime import datetime, timezone
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "sessions": self._last_seq_by_session,
        }
        try:
            self.cursor_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except OSError:
            pass

    def _persist_discovered(self) -> None:
        import json
        from datetime import datetime, timezone
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "onboard_ip": self._last_onboard_ip,
            "session_id": self._last_onboard_session,
            "command_port": self.command_port,
            "telemetry_port": self.port,
        }
        try:
            self.discovered_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except OSError:
            pass

    def _discovery_loop(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("", self.discovery_port))
            sock.settimeout(0.5)

            while not self._stop.is_set():
                nonce = str(int(time.time() * 1000))
                hello = f"GS_HELLO,{nonce},{self.port},{self.command_port}\n"
                beacon = f"GS_BEACON,{nonce},{self.port},{self.command_port},100\n"
                for line in (hello, beacon):
                    for target in ("255.255.255.255", DEFAULT_STATIC_ONBOARD_HOST):
                        try:
                            sock.sendto(line.encode("utf-8"), (target, self.discovery_port))
                        except OSError:
                            pass

                end_time = time.time() + 1.0
                while time.time() < end_time and not self._stop.is_set():
                    try:
                        data, addr = sock.recvfrom(2048)
                    except socket.timeout:
                        continue
                    except OSError:
                        break

                    line = data.decode("utf-8", errors="replace").strip()
                    parts = [p.strip() for p in line.split(",")]
                    if parts[0] == "ONBOARD_HELLO" and len(parts) >= 6 and parts[1] == nonce:
                        session = parts[2]
                    elif parts[0] == "ONBOARD_BEACON" and len(parts) >= 5:
                        session = parts[1]
                    else:
                        continue

                    with self._lock:
                        self._last_onboard_ip = addr[0]
                        self._last_onboard_session = session
                        self._persist_discovered()
                    print(f"[discovery] onboard={addr[0]} session={session}")

    def _network_loop(self) -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((self.bind, self.port))
                server.listen(1)
                server.settimeout(1.0)
                print(f"[telemetry] listening on {self.bind}:{self.port}")

                while not self._stop.is_set():
                    try:
                        conn, addr = server.accept()
                    except socket.timeout:
                        self._log_waiting()
                        continue

                    print(f"[telemetry] onboard connected from {addr[0]}:{addr[1]}")
                    try:
                        self._handle_connection(conn)
                    finally:
                        conn.close()
                        print("[telemetry] onboard disconnected")
        finally:
            self._stop.set()

    def _cycle_writer(self) -> Optional[CsvAppender]:
        """`cycles.csv` lives beside the session's other files; the writer is
        re-created whenever the session directory changes."""
        current = self.logs.current_dir
        if current is None:
            return None
        if self._cycles is None or self._cycles_dir != current:
            if self._cycles is not None:
                self._cycles.close()
            self._cycles = CsvAppender(current / "cycles.csv", CYCLE_CSV_FIELDS)
            self._cycles_dir = current
        return self._cycles

    def _handle_connection(self, conn: socket.socket) -> None:
        buffer = ""
        conn.settimeout(1.0)
        timeout_warned = False
        while not self._stop.is_set():
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                if (
                    self.timeout_s > 0
                    and self._last_packet_time > 0
                    and (time.time() - self._last_packet_time) > self.timeout_s
                ):
                    if not timeout_warned:
                        print(
                            f"[alert] telemetry timeout > {self.timeout_s:.1f}s, "
                            "closing stale connection"
                        )
                        timeout_warned = True
                    return
                continue

            if not chunk:
                break

            buffer += chunk.decode("utf-8", errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                rx_utc = utc_now_iso()
                if line.startswith("EVT,CYCLE,"):
                    try:
                        event = parse_heating_cycle_event(line)
                    except TelemetryParseError as exc:
                        print(f"[telemetry][evt-parse-error] {exc}: {line}")
                        continue
                    self._last_packet_time = time.time()
                    self._append_cycle(event, line, rx_utc)
                    ack_line = build_ack(event.session_id, 0)
                    try:
                        conn.sendall(ack_line.encode("utf-8"))
                    except OSError:
                        return
                    print(
                        f"[evt][cycle] session={event.session_id} cycle={event.cycle_id} "
                        f"specimen={event.specimen_index} peak={event.peak_temp_c:.2f}C "
                        f"hold={event.hold_duration_s:.1f}s "
                        f"cool={event.cooldown_rate_c_per_s:.3f}C/s"
                    )
                    continue

                if line.startswith("EVT,PULL,"):
                    try:
                        pull = parse_pull_event(line)
                    except TelemetryParseError as exc:
                        print(f"[telemetry][evt-parse-error] {exc}: {line}")
                        continue
                    self._last_packet_time = time.time()
                    # EVT,PULL is keyed by (session, pull_id) so a replay
                    # from the onboard queue never lands twice in pulls.csv.
                    dup_key = (pull.session_id, int(pull.pull_id))
                    with self._lock:
                        is_dup_pull = dup_key in self._seen_pull_ids
                        if not is_dup_pull:
                            self._seen_pull_ids.add(dup_key)
                    if not is_dup_pull:
                        self.logs.on_pull(pull, rx_utc)
                    # ACK with seq 0: removes exactly the queued event frame
                    # and never touches DATA frames (their seqs start at 1).
                    ack_line = build_ack(pull.session_id, 0)
                    try:
                        conn.sendall(ack_line.encode("utf-8"))
                    except OSError:
                        return
                    samples_str = "|".join(str(s) for s in pull.samples) or "-"
                    print(
                        f"[evt][pull] session={pull.session_id} pull={pull.pull_id} "
                        f"motor={pull.motor_id} steps={pull.steps_moved} "
                        f"hold={pull.hold_s:.1f}s samples={samples_str}"
                    )
                    continue

                try:
                    packet = parse_telemetry_csv(line)
                except TelemetryParseError as exc:
                    print(f"[telemetry][parse-error] {exc}: {line}")
                    continue

                self._last_packet_time = time.time()
                # No box sensor. Use the hottest sample reading as the
                # over-temperature trigger instead.
                valid_temps = [
                    value for i, value in enumerate(packet.sample_temps_c)
                    if packet.sensor_valid.get(f"S{i}", True)
                    and math.isfinite(value)
                ]
                hot = max(valid_temps) if valid_temps else None
                if hot is not None and hot > self.alert_temp_c:
                    print(f"[alert] sample temp high: {hot:.2f} C")

                with self._lock:
                    last_seq = self._last_seq_by_session.get(packet.session_id, -1)
                    is_duplicate = packet.seq <= last_seq
                    if not is_duplicate:
                        self._last_seq_by_session[packet.session_id] = packet.seq
                        self._persist_cursor()

                ack_line = build_ack(packet.session_id, packet.seq)
                try:
                    conn.sendall(ack_line.encode("utf-8"))
                except OSError:
                    return

                if is_duplicate:
                    print(f"[telemetry] duplicate dropped session={packet.session_id} seq={packet.seq}")
                    continue

                if self.logs.on_packet(packet, rx_utc):
                    print(f"[log] session {packet.session_id} -> {self.logs.current_dir}")

                if self._plotter is not None:
                    plot_temp = (
                        packet.ambient_temp_c
                        if packet.sensor_valid.get("AT", True) and
                        math.isfinite(packet.ambient_temp_c)
                        else math.nan
                    )
                    plot_pressure = (
                        packet.ambient_pressure_mbar
                        if packet.sensor_valid.get("AP", True) and
                        math.isfinite(packet.ambient_pressure_mbar)
                        else math.nan
                    )
                    self._plotter.push(packet.seq, plot_temp, plot_pressure)

                hot_str = f"{hot:.2f}C" if hot is not None else "—"
                pressure_text = (
                    f"{packet.ambient_pressure_mbar:.1f}mbar"
                    if packet.sensor_valid.get("AP", True) and
                    math.isfinite(packet.ambient_pressure_mbar)
                    else "N/A"
                )
                print(
                    f"[telemetry] session={packet.session_id} seq={packet.seq} phase={packet.phase} "
                    f"P={pressure_text} Thot={hot_str}"
                )

    def _append_cycle(self, event: HeatingCycleEvent, raw_line: str, rx_utc: str) -> None:
        writer = self._cycle_writer()
        if writer is None:
            # No session directory yet (a cycle event before the first DATA
            # frame): open one for this session so the event is not lost.
            self.logs.log_event("INFO", f"cycle event before first frame: {raw_line}")
            return
        try:
            writer.write({
                "gs_rx_utc": rx_utc,
                "session_id": event.session_id,
                "cycle_id": event.cycle_id,
                "start_ts": event.start_ts,
                "peak_temp_c": f"{event.peak_temp_c:.2f}",
                "hold_duration_s": f"{event.hold_duration_s:.2f}",
                "cooldown_rate_c_per_s": f"{event.cooldown_rate_c_per_s:.4f}",
                "specimen_index": event.specimen_index,
                "raw": raw_line,
            })
        except OSError as exc:
            print(f"[evt][log-error] {exc}")

    def _log_waiting(self) -> None:
        now = time.time()
        if (now - self._last_wait_log_time) >= 5.0:
            print("[telemetry] waiting for onboard TCP connection...")
            self._last_wait_log_time = now


def add_subparser(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser("telemetry-server", help="Run telemetry receiver")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=4000)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG_ROOT,
                        help="Log root; every onboard session gets its own directory "
                             "under <root>/sessions/ (same layout as the GUI).")
    parser.add_argument("--plot", action="store_true", help="Enable live matplotlib plot")
    parser.add_argument("--alert-temp-c", type=float, default=80.0)
    parser.add_argument("--timeout-s", type=float, default=10.0)
    parser.add_argument("--discovery-enabled", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--discovery-port", type=int, default=4100)
    parser.add_argument("--command-port", type=int, default=5000)
    parser.add_argument("--cursor", type=Path, default=Path("logs/ground_ack_cursor.json"))
    parser.add_argument("--discovered", type=Path, default=Path("logs/discovered_onboard.json"))
    parser.set_defaults(_coatheal_handler=_handle)


def _handle(args: argparse.Namespace) -> int:
    import signal
    server = TelemetryServer(
        bind=args.bind,
        port=args.port,
        log_root=args.log,
        plot=args.plot,
        alert_temp_c=args.alert_temp_c,
        timeout_s=args.timeout_s,
        discovery_enabled=args.discovery_enabled,
        discovery_port=args.discovery_port,
        command_port=args.command_port,
        cursor_path=args.cursor,
        discovered_path=args.discovered,
    )
    # SIGTERM (systemd stop, `timeout`, a supervisor) must close the session
    # files as cleanly as Ctrl+C does.
    try:
        signal.signal(signal.SIGTERM, lambda _signum, _frame: server.stop())
    except (ValueError, OSError):
        pass
    try:
        server.run()
        return 0
    except KeyboardInterrupt:
        print("\n[telemetry] stopping")
        return 0
