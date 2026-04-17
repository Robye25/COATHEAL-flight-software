"""Regression coverage for the telemetry-server CSV expansion (Rev B.1 v5).

Added 2026-04-17 by Agent C after discovering that the CSV header written
by `TelemetryServer._handle_connection` was missing `mode`, `sample_0..7`,
`h0..5`, and `stepperN_*` columns even though the parsed packet carries all
of them. This test drives the server against a synthetic onboard TCP peer
(loopback) so it catches regressions in both the header and the row writer.
"""
from __future__ import annotations

import csv
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.telemetry_server import TelemetryServer  # noqa: E402


FRAME = (
    "DATA,sess-csv,3,2026-04-17T10:00:00Z,1,-10.00,140.00,0.01,"
    "-5.00,-5.10,-5.20,-5.30,-5.40,-5.50,-5.60,-5.70,"
    "HEATER_DUTY=0.00|0.10|0.20|0.30|0.40|0.50,"
    "RESISTANCE=10.5|11.0|9.8|10.1|10.7|10.3|-|-,"
    "PHASE=FLOAT,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK"
    "|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_INHIBITED|RESISTANCE_OK,"
    "STEPPER0=pos:100|tgt:200|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:100|src:cmd:MOVE,"
    "STEPPER1=pos:-50|tgt:-50|hz:200|us:8|en:1|mv:0|hold:1|hold_s:3.5|pulses:50|src:phase:FLOAT\n"
)


class CsvHeaderTests(unittest.TestCase):
    def test_csv_contains_all_rev_b1_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            server = TelemetryServer(
                bind="127.0.0.1",
                port=0,  # overridden below via a pre-bound socket
                log_path=tmp / "gt.csv",
                plot=False,
                alert_temp_c=80.0,
                timeout_s=5.0,
                discovery_enabled=False,
                discovery_port=0,
                command_port=0,
                cursor_path=tmp / "cursor.json",
                discovered_path=tmp / "discovered.json",
            )
            # Run the server's network loop on an ephemeral port. We pick
            # the port, then patch server.port so the listener binds there.
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.bind(("127.0.0.1", 0))
            server.port = probe.getsockname()[1]
            probe.close()

            net_thread = threading.Thread(target=server._network_loop, daemon=True)
            net_thread.start()

            # Wait for listener up.
            deadline = time.time() + 2.0
            conn: socket.socket | None = None
            while time.time() < deadline:
                try:
                    conn = socket.create_connection(("127.0.0.1", server.port), timeout=1.0)
                    break
                except OSError:
                    time.sleep(0.05)
            self.assertIsNotNone(conn, "server did not accept connections")
            assert conn is not None  # narrow Optional for type checkers
            try:
                conn.sendall(FRAME.encode("utf-8"))
                # Read the ACK to ensure the server processed the frame.
                conn.settimeout(2.0)
                ack = conn.recv(256).decode("utf-8", errors="replace")
                self.assertTrue(ack.startswith("ACK,sess-csv,3"), ack)
            finally:
                conn.close()

            # Give the writer a moment to flush.
            time.sleep(0.2)
            server.stop()
            net_thread.join(timeout=2.0)

            csv_path = tmp / "gt.csv"
            self.assertTrue(csv_path.exists(), "csv not written")
            with csv_path.open("r", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                rows = list(reader)

        self.assertEqual(len(rows), 1, f"expected one row, got {len(rows)}")
        row = rows[0]

        # Scalar fields.
        for col in (
            "session_id", "seq", "timestamp", "rtc_valid",
            "ambient_temp_c", "ambient_pressure_mbar", "uv",
            "phase", "mode", "status",
        ):
            self.assertIn(col, row, f"missing column {col}")
            self.assertNotEqual(row[col].strip(), "",
                                f"column {col} is blank")

        # 8 per-sample columns.
        for i in range(8):
            col = f"sample_{i}"
            self.assertIn(col, row)
            self.assertNotEqual(row[col].strip(), "", f"{col} blank")

        # 6 heater columns.
        for i in range(6):
            col = f"h{i}"
            self.assertIn(col, row)
            self.assertNotEqual(row[col].strip(), "", f"{col} blank")

        # 8 resistance columns; r6 and r7 must be literal "-" placeholders.
        for i in range(8):
            col = f"r{i}"
            self.assertIn(col, row)
            self.assertNotEqual(row[col].strip(), "", f"{col} blank")
        self.assertEqual(row["r6"], "-")
        self.assertEqual(row["r7"], "-")

        # Per-motor stepper segments expanded into columns.
        for m in (0, 1):
            for suffix in ("position", "target", "hz", "microstep",
                           "enabled", "moving", "holding", "hold_s",
                           "pulses"):
                col = f"stepper{m}_{suffix}"
                self.assertIn(col, row)
                self.assertNotEqual(row[col].strip(), "", f"{col} blank")

        # Spot-check values parsed correctly.
        self.assertEqual(row["mode"], "RUN")
        self.assertEqual(row["phase"], "FLOAT")
        self.assertEqual(row["stepper0_position"], "100")
        self.assertEqual(row["stepper1_position"], "-50")


if __name__ == "__main__":
    unittest.main()
