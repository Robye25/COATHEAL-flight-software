"""Both telemetry receivers -- the GUI's TelemetryReceiver and the headless
TelemetryServer -- against a fake onboard on loopback: the HELLO codec
negotiation, `Z1,` frames handled exactly like plain ones (same ACKs, same
logs), and a broken `Z1,` frame counted as a parse error and never ACKed."""
from __future__ import annotations

import csv
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import free_port  # noqa: E402
from test_link_codec import ONBOARD_LINE, other_frame  # noqa: E402

from app.link_codec import DICTIONARY_CRC, encode_line  # noqa: E402

SESSION = "coatheal-1789498045-582267"
PULL = f"EVT,PULL,{SESSION},7,1,2026-09-15T18:48:10Z,1600,5.00,4|5|6|7,4"
PLAIN_NEXT = ONBOARD_LINE.replace(f"{SESSION},547,", f"{SESSION},548,")


class LineReader:
    def __init__(self, conn: socket.socket) -> None:
        self._conn = conn
        self._buf = b""

    def line(self) -> str:
        while b"\n" not in self._buf:
            chunk = self._conn.recv(4096)
            if not chunk:
                raise AssertionError(f"connection closed; unread {self._buf!r}")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8")


def connect(port: int, timeout_s: float = 3.0) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            conn = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            conn.settimeout(3.0)
            return conn
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)


def speak_z1(test: unittest.TestCase, port: int) -> None:
    """The fake onboard: negotiate z1, send a compressed DATA and EVT,PULL, a
    broken frame, then a plain DATA frame."""
    with connect(port) as conn:
        reader = LineReader(conn)
        conn.sendall(f"HELLO,{SESSION},z1:{DICTIONARY_CRC}\n".encode())
        test.assertEqual(reader.line(), "HELLO,z1")
        conn.sendall(f"{encode_line(ONBOARD_LINE)}\n".encode())
        test.assertEqual(reader.line(), f"ACK,{SESSION},547")
        conn.sendall(f"{encode_line(PULL)}\n".encode())
        test.assertEqual(reader.line(), f"ACK,{SESSION},0")
        conn.sendall(b"Z1,@@@@not-base64\n")
        conn.sendall(f"{PLAIN_NEXT}\n".encode())
        test.assertEqual(reader.line(), f"ACK,{SESSION},548", "the broken Z1 frame must get no ACK")


def session_files(log_root: Path) -> tuple:
    session_dir = next((log_root / "sessions").iterdir())
    with (session_dir / "telemetry.csv").open(encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))
    with (session_dir / "pulls.csv").open(encoding="utf-8", newline="") as fh:
        pulls = list(csv.DictReader(fh))
    events = (session_dir / "events.log").read_text(encoding="utf-8")
    return rows, pulls, events


class GuiReceiverZ1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as exc:
            raise unittest.SkipTest(f"no Qt platform: {exc}")

    def test_hello_and_z1_frames(self) -> None:
        from PyQt6.QtCore import Qt
        from app.gui.dispatch import TelemetryReceiver
        from app.telemetry_log import LogManager
        direct = Qt.ConnectionType.DirectConnection
        with tempfile.TemporaryDirectory() as tmp:
            logs = LogManager(Path(tmp) / "logs")
            port = free_port()
            receiver = TelemetryReceiver("127.0.0.1", port, logs)
            packets: list = []
            pulls: list = []
            messages: list = []
            listening = threading.Event()
            receiver.packet_received.connect(packets.append, direct)
            receiver.pull_event.connect(pulls.append, direct)
            receiver.log_message.connect(messages.append, direct)
            receiver.status_changed.connect(lambda state: state == "listening" and listening.set(), direct)
            receiver.start()
            try:
                self.assertTrue(listening.wait(3.0), "receiver never listened")
                speak_z1(self, port)
            finally:
                receiver.stop()
                receiver.wait(3000)
                logs.close()
            self.assertEqual([(p.session_id, p.seq) for p in packets], [(SESSION, 547), (SESSION, 548)])
            self.assertEqual(packets[0].steppers[1]["mm"], 2.0)
            self.assertEqual([p.pull_id for p in pulls], [7])
            self.assertEqual(receiver.parse_errors, 1, "the broken frame, and not the HELLO, is a parse error")
            self.assertTrue(any("HELLO,z1" in m for m in messages), messages)
            rows, pull_rows, events = session_files(Path(tmp) / "logs")
            self.assertEqual([r["seq"] for r in rows], ["547", "548"])
            self.assertEqual(len(pull_rows), 1)
            self.assertIn("Z1,@@@@not-base64", events, "the raw broken line is kept in the event log")
            self.assertNotIn("HELLO", events.replace("HELLO,z1", ""), "the HELLO is not an unparseable frame")

    # MUTATION: remove the `if line.startswith("Z1,"):` block from
    # TelemetryReceiver._handle_connection and confirm test_hello_and_z1_frames
    # fails waiting for ACK,<session>,547.

    def test_mismatched_dictionary_answers_plain(self) -> None:
        from app.gui.dispatch import TelemetryReceiver
        from app.telemetry_log import LogManager
        with tempfile.TemporaryDirectory() as tmp:
            logs = LogManager(Path(tmp) / "logs")
            port = free_port()
            receiver = TelemetryReceiver("127.0.0.1", port, logs)
            receiver.start()
            try:
                with connect(port) as conn:
                    reader = LineReader(conn)
                    conn.sendall(f"HELLO,{SESSION},z1:00000000\n".encode())
                    self.assertEqual(reader.line(), "HELLO,plain")
                    conn.sendall(f"{ONBOARD_LINE}\n".encode())
                    self.assertEqual(reader.line(), f"ACK,{SESSION},547")
            finally:
                receiver.stop()
                receiver.wait(3000)
                logs.close()
            self.assertEqual(receiver.parse_errors, 0)


    def test_a_quiet_connection_is_closed_only_once_its_fin_is_charged(self) -> None:
        from app.gui.dispatch import TelemetryReceiver
        from app.link_budget import GROUND_SHARE, TELEMETRY_CLOSE_BYTES, LinkBudget, Priority
        from app.telemetry_log import LogManager
        with tempfile.TemporaryDirectory() as tmp:
            logs = LogManager(Path(tmp) / "logs")
            port = free_port()
            budget = LinkBudget(GROUND_SHARE)
            full = budget.hold(GROUND_SHARE, Priority.CRITICAL)
            receiver = TelemetryReceiver("127.0.0.1", port, logs, budget=budget)
            receiver._DATA_TIMEOUT_S = 0.5
            receiver.start()
            try:
                with connect(port) as conn:
                    conn.settimeout(2.5)
                    with self.assertRaises(socket.timeout, msg="no room for the FIN: the quiet link stays open"):
                        conn.recv(1)
                    budget.release(full)
                    conn.settimeout(5.0)
                    self.assertEqual(conn.recv(1), b"", "closed once the FIN and its ACK fit the budget")
                self.assertGreaterEqual(budget.in_window(), TELEMETRY_CLOSE_BYTES)
            finally:
                receiver.stop()
                receiver.wait(3000)
                logs.close()


class ServerZ1Tests(unittest.TestCase):
    def _server(self, tmp: Path):
        from app.telemetry_server import TelemetryServer
        return TelemetryServer(
            bind="127.0.0.1", port=free_port(), log_root=tmp / "logs", plot=False, alert_temp_c=80.0,
            timeout_s=5.0, discovery_enabled=False, discovery_port=0, command_port=0,
            cursor_path=tmp / "cursor.json", discovered_path=tmp / "discovered.json")

    def test_hello_and_z1_frames(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            server = self._server(tmp)
            thread = threading.Thread(target=server._network_loop, daemon=True)
            thread.start()
            try:
                speak_z1(self, server.port)
                # A second session's compressed frame through the same path.
                with connect(server.port) as conn:
                    reader = LineReader(conn)
                    conn.sendall(f"{encode_line(other_frame())}\n".encode())
                    self.assertEqual(reader.line(), "ACK,coatheal-1790012345-424242,31337")
            finally:
                time.sleep(0.1)
                server.stop()
                thread.join(3.0)
                server.logs.close()
            sessions = sorted(p.name for p in (tmp / "logs" / "sessions").iterdir())
            self.assertEqual(len(sessions), 2, sessions)
            first = next(p for p in (tmp / "logs" / "sessions").iterdir() if p.name.endswith(SESSION))
            with (first / "telemetry.csv").open(encoding="utf-8", newline="") as fh:
                self.assertEqual([r["seq"] for r in csv.DictReader(fh)], ["547", "548"])
            with (first / "pulls.csv").open(encoding="utf-8", newline="") as fh:
                self.assertEqual([r["pull_id"] for r in csv.DictReader(fh)], ["7"])
            self.assertIn("Z1,@@@@not-base64", (first / "events.log").read_text(encoding="utf-8"))

    # MUTATION: answer every HELLO with "HELLO,plain\n" in
    # TelemetryServer._handle_connection and confirm this test fails on "HELLO,z1".


if __name__ == "__main__":
    unittest.main()
