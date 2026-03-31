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

from .protocol import TelemetryParseError, build_ack, parse_telemetry_csv


class LivePlotter:
    def __init__(self) -> None:
        import matplotlib.pyplot as plt

        plt.ion()
        self._plt = plt
        # Thread-safe: network thread appends, main thread pops
        self._buf: collections.deque = collections.deque()
        self._seq: list[int] = []
        self._box_temp: list[float] = []
        self._pressure: list[float] = []

        self._fig, (self._ax_temp, self._ax_pressure) = plt.subplots(2, 1, figsize=(10, 7))

        (self._temp_line,) = self._ax_temp.plot([], [], label="Box Temp [C]")
        self._ax_temp.set_ylabel("Temperature [C]")
        self._ax_temp.grid(True)
        self._ax_temp.legend(loc="best")

        (self._pressure_line,) = self._ax_pressure.plot([], [], label="Ambient Pressure [mbar]")
        self._ax_pressure.set_ylabel("Pressure [mbar]")
        self._ax_pressure.set_xlabel("SEQ")
        self._ax_pressure.grid(True)
        self._ax_pressure.legend(loc="best")

        self._fig.tight_layout()

    def push(self, seq: int, box_temp_c: float, pressure_mbar: float) -> None:
        """Called from network thread. deque.append is thread-safe."""
        self._buf.append((seq, box_temp_c, pressure_mbar))

    def tick(self) -> None:
        """Called from main thread only. Drains buffer and redraws at up to 20 fps."""
        changed = False
        while self._buf:
            seq, temp, pres = self._buf.popleft()
            self._seq.append(seq)
            self._box_temp.append(temp)
            self._pressure.append(pres)
            changed = True

        if changed:
            if len(self._seq) > 600:
                self._seq = self._seq[-600:]
                self._box_temp = self._box_temp[-600:]
                self._pressure = self._pressure[-600:]

            self._temp_line.set_data(self._seq, self._box_temp)
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
                try:
                    sock.sendto(hello.encode("utf-8"), ("255.255.255.255", self.discovery_port))
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
                    if len(parts) < 6 or parts[0] != "ONBOARD_HELLO":
                        continue
                    if parts[1] != nonce:
                        continue

                    with self._lock:
                        self._last_onboard_ip = addr[0]
                        self._last_onboard_session = parts[2]
                        self._persist_discovered()
                    print(f"[discovery] onboard={addr[0]} session={parts[2]}")

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
                    "ambient_humidity_pct",
                    "uv",
                    "box_temp_c",
                    "sample_temps_c",
                    "heater_duty",
                    "phase",
                    "status",
                ],
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
                    try:
                        packet = parse_telemetry_csv(line)
                    except TelemetryParseError as exc:
                        print(f"[telemetry][parse-error] {exc}: {line}")
                        continue

                    self._last_packet_time = time.time()
                    if packet.box_temp_c > self.alert_temp_c:
                        print(f"[alert] box temp high: {packet.box_temp_c:.2f} C")

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
                    writer.writerow(row)
                    f.flush()

                    if self._plotter is not None:
                        self._plotter.push(packet.seq, packet.box_temp_c, packet.ambient_pressure_mbar)

                    print(
                        f"[telemetry] session={packet.session_id} seq={packet.seq} phase={packet.phase} "
                        f"P={packet.ambient_pressure_mbar:.1f}mbar Tbox={packet.box_temp_c:.2f}C"
                    )

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
