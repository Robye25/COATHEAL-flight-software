from __future__ import annotations

import argparse
import collections
import csv
import json
import socket
import threading
import time
from dataclasses import asdict
from datetime import datetime, timezone
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

DEFAULT_STATIC_ONBOARD_HOST = "169.254.10.10"


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
    def __init__(
        self,
        bind: str,
        port: int,
        log_path: Path,
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
        self.log_path = log_path
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
        self._last_onboard_ip = ""
        self._last_onboard_session = ""

        self._load_cursor()

    def run(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
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

    def stop(self) -> None:
        self._stop.set()

    def _load_cursor(self) -> None:
        if not self.cursor_path.exists():
            return

        try:
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
        except (OSError, ValueError, json.JSONDecodeError):
            self._last_seq_by_session = {}

    def _persist_cursor(self) -> None:
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "sessions": self._last_seq_by_session,
        }
        try:
            self.cursor_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except OSError:
            pass

    def _persist_discovered(self) -> None:
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

    def _handle_connection(self, conn: socket.socket) -> None:
        buffer = ""
        first = not self.log_path.exists()

        # CSV v5: adds `mode` + per-sample `sample_0..7` columns and
        # per-motor `stepperN_*` columns so an offline analyst can answer "is
        # HEATER_INHIBITED because motor 0 is moving?" from the CSV alone
        # without re-parsing the raw frame (Agent C, 2026-04-17).
        sample_cols = [f"sample_{i}" for i in range(8)]
        heater_cols = [f"h{i}" for i in range(6)]
        r_cols = [f"r{i}" for i in range(8)]
        stepper_cols = []
        for m in (0, 1):
            for suffix in (
                "position", "target", "hz", "microstep",
                "enabled", "moving", "holding", "hold_s", "pulses",
            ):
                stepper_cols.append(f"stepper{m}_{suffix}")
        with self.log_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "session_id",
                    "seq",
                    "timestamp",
                    "rtc_valid",
                    "ambient_temp_c",
                    "ambient_pressure_mbar",
                    "uv",
                    "sample_temps_c",
                    *sample_cols,
                    "heater_duty",
                    *heater_cols,
                    *r_cols,
                    "phase",
                    "mode",
                    "status",
                    *stepper_cols,
                ],
                # `extrasaction='ignore'` lets us pass the full asdict()
                # without it raising on the additional in-memory fields
                # (`steppers`, `stepper`, …).
                extrasaction="ignore",
            )
            if first:
                writer.writeheader()

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
                    if line.startswith("EVT,CYCLE,"):
                        try:
                            event = parse_heating_cycle_event(line)
                        except TelemetryParseError as exc:
                            print(f"[telemetry][evt-parse-error] {exc}: {line}")
                            continue
                        self._last_packet_time = time.time()
                        self._append_event_log(event, line)
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
                        # Track
                        # EVT,PULL by (session, pull_id) so the same event
                        # replayed from the onboard queue doesn't land in
                        # the pulls CSV multiple times. Key is independent
                        # of the telemetry-queue seq, which we don't see on
                        # the wire.
                        dup_key = (pull.session_id, int(pull.pull_id))
                        with self._lock:
                            seen_pulls = getattr(
                                self, "_seen_pull_ids",
                                None)
                            if seen_pulls is None:
                                seen_pulls = set()
                                self._seen_pull_ids = seen_pulls
                            is_dup_pull = dup_key in seen_pulls
                            if not is_dup_pull:
                                seen_pulls.add(dup_key)
                        if not is_dup_pull:
                            self._append_pull_log(pull, line)
                        # Rev C fix: the previous 2**63-1 ACK was nuclear —
                        # it caused Acknowledge() to delete ALL queued frames
                        # (seq <= 2^63-1 is always true). ACK with 0 is safe:
                        # it won't remove any queued DATA frames (their seqs
                        # start at 1). EVT,PULL events are already deduplicated
                        # on the ground by (session_id, pull_id), so replays
                        # from the queue are harmless. The proper long-term fix
                        # is to embed the queue seq in the EVT wire format or
                        # give EVT frames a separate dequeue path.
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
                    # No box sensor. Use the hottest sample
                    # reading as the over-temperature trigger instead.
                    hot = max(packet.sample_temps_c) if packet.sample_temps_c else None
                    if hot is not None and hot > self.alert_temp_c:
                        print(f"[alert] sample temp high: {hot:.2f} C")

                    is_duplicate = False
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

                    row = asdict(packet)
                    row["sample_temps_c"] = "|".join(f"{x:.2f}" for x in packet.sample_temps_c)
                    row["heater_duty"] = "|".join(f"{x:.3f}" for x in packet.heater_duty)
                    # Also emit one column per sample/heater so the
                    # CSV is self-describing and easy to plot directly. Fills
                    # a `-` placeholder when the wire frame is short so no
                    # downstream column is blank in a well-formed packet.
                    for i in range(8):
                        if i < len(packet.sample_temps_c):
                            row[f"sample_{i}"] = f"{packet.sample_temps_c[i]:.2f}"
                        else:
                            row[f"sample_{i}"] = "-"
                    for i in range(6):
                        if i < len(packet.heater_duty):
                            row[f"h{i}"] = f"{packet.heater_duty[i]:.3f}"
                        else:
                            row[f"h{i}"] = "-"
                    # Resistance: one column per sample. Unmeasured channels
                    # (wire placeholder "-") map to `None` in the packet, and
                    # render here as "-" so the column is never blank.
                    for i in range(8):
                        if i < len(packet.sample_resistance_ohm):
                            v = packet.sample_resistance_ohm[i]
                            row[f"r{i}"] = "-" if v is None else f"{v:.3f}"
                        else:
                            row[f"r{i}"] = "-"
                    # Per-motor stepper snapshot expansion. `steppers` is a
                    # list of dicts ordered by motor id; we emit only the
                    # slots the frame carries — missing motors render as "-".
                    motor_snaps = {s.get("motor_id", i): s
                                   for i, s in enumerate(packet.steppers)}
                    for m in (0, 1):
                        snap = motor_snaps.get(m)
                        for key, suffix in (
                            ("position", "position"),
                            ("target", "target"),
                            ("hz", "hz"),
                            ("microstep", "microstep"),
                            ("enabled", "enabled"),
                            ("moving", "moving"),
                            ("holding", "holding"),
                            ("hold_s", "hold_s"),
                            ("pulses", "pulses"),
                        ):
                            val = snap.get(key) if snap is not None else None
                            if val is None:
                                row[f"stepper{m}_{suffix}"] = "-"
                            elif isinstance(val, bool):
                                row[f"stepper{m}_{suffix}"] = "1" if val else "0"
                            elif isinstance(val, float):
                                row[f"stepper{m}_{suffix}"] = f"{val:.3f}"
                            else:
                                row[f"stepper{m}_{suffix}"] = str(val)
                    writer.writerow(row)
                    f.flush()

                    if self._plotter is not None:
                        self._plotter.push(packet.seq, packet.ambient_temp_c, packet.ambient_pressure_mbar)

                    hot_str = f"{hot:.2f}C" if hot is not None else "—"
                    print(
                        f"[telemetry] session={packet.session_id} seq={packet.seq} phase={packet.phase} "
                        f"P={packet.ambient_pressure_mbar:.1f}mbar Thot={hot_str}"
                    )

    def _append_pull_log(self, pull: PullEvent, raw_line: str) -> None:
        """Write a pull-cycle event to a sibling `<log>_pulls.csv` file.

        Kept separate from the cycle events log so the two streams can be
        analyzed independently post-flight. Header is written on first row
        only so appending to an existing file remains valid.
        """
        pull_path = self.log_path.with_name(self.log_path.stem + "_pulls.csv")
        first = not pull_path.exists()
        try:
            with pull_path.open("a", newline="", encoding="utf-8") as pf:
                writer = csv.writer(pf)
                if first:
                    writer.writerow([
                        "session_id",
                        "pull_id",
                        "motor_id",
                        "start_ts",
                        "steps_moved",
                        "hold_s",
                        "samples",
                        "raw",
                    ])
                writer.writerow([
                    pull.session_id,
                    pull.pull_id,
                    pull.motor_id,
                    pull.start_ts,
                    pull.steps_moved,
                    f"{pull.hold_s:.2f}",
                    "|".join(str(s) for s in pull.samples),
                    raw_line,
                ])
        except OSError as exc:
            print(f"[evt][pull-log-error] {exc}")

    def _append_event_log(self, event, raw_line: str) -> None:
        event_path = self.log_path.with_name(self.log_path.stem + "_events.csv")
        first = not event_path.exists()
        try:
            with event_path.open("a", newline="", encoding="utf-8") as ef:
                writer = csv.writer(ef)
                if first:
                    writer.writerow([
                        "session_id",
                        "cycle_id",
                        "start_ts",
                        "peak_temp_c",
                        "hold_duration_s",
                        "cooldown_rate_c_per_s",
                        "specimen_index",
                        "raw",
                    ])
                writer.writerow([
                    event.session_id,
                    event.cycle_id,
                    event.start_ts,
                    f"{event.peak_temp_c:.2f}",
                    f"{event.hold_duration_s:.2f}",
                    f"{event.cooldown_rate_c_per_s:.4f}",
                    event.specimen_index,
                    raw_line,
                ])
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
    parser.add_argument("--log", type=Path, default=Path("logs/ground_telemetry.csv"))
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
    server = TelemetryServer(
        bind=args.bind,
        port=args.port,
        log_path=args.log,
        plot=args.plot,
        alert_temp_c=args.alert_temp_c,
        timeout_s=args.timeout_s,
        discovery_enabled=args.discovery_enabled,
        discovery_port=args.discovery_port,
        command_port=args.command_port,
        cursor_path=args.cursor,
        discovered_path=args.discovered,
    )
    try:
        server.run()
        return 0
    except KeyboardInterrupt:
        print("\n[telemetry] stopping")
        return 0
