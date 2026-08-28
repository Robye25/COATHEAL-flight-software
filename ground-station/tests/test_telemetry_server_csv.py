"""The CLI telemetry server writes exactly what the GUI writes: schema v6
through `telemetry_log.LogManager`, in a per-session directory. This test
drives the server against a synthetic onboard TCP peer (loopback) so it
catches regressions in the wiring, not just in the row builder (which
`test_telemetry_log.py` covers directly).
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

from app.telemetry_log import TELEMETRY_CSV_FIELDS  # noqa: E402
from app.telemetry_server import TelemetryServer  # noqa: E402


FRAME = (
    "DATA,sess-csv,3,2026-04-17T10:00:00Z,1,-10.00,140.00,0.01,"
    "-5.00,-5.10,-5.20,-5.30,-5.40,-5.50,-5.60,-5.70,"
    "HEATER_DUTY=0.00|0.10|0.20|0.30|0.40|0.50,"
    "RESISTANCE=10.5|11.0|9.8|10.1|10.7|10.3|-|-,"
    "PHASE=FLOAT,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK"
    "|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|HEATER_INHIBITED|RESISTANCE_OK,"
    "CTRL=fallback:0|link_loss_s:0.0|energy_wh:1.5|budget_wh:130.0|budget_exhausted:0"
    "|heaters_active:3|queue:0|plan:none,"
    "STEPPER0=pos:100|tgt:200|hz:100|us:4|en:1|mv:1|hold:0|hold_s:0|pulses:100|src:cmd:MOVE"
    "|zeroed:1|seq:-|seqst:idle,"
    "STEPPER1=pos:-50|tgt:-50|hz:100|us:4|en:1|mv:0|hold:1|hold_s:3.5|pulses:50|src:cmd:BEND"
    "|zeroed:1|seq:flex|seqst:run\n"
)
PULL = "EVT,PULL,sess-csv,7,1,2026-04-17T10:00:05Z,800,5.00,4|5|6|7\n"


class CsvHeaderTests(unittest.TestCase):
    def test_server_writes_schema_v6_session_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            server = TelemetryServer(
                bind="127.0.0.1",
                port=0,  # overridden below via a pre-bound socket
                log_root=tmp / "logs",
                plot=False,
                alert_temp_c=80.0,
                timeout_s=5.0,
                discovery_enabled=False,
                discovery_port=0,
                command_port=0,
                cursor_path=tmp / "cursor.json",
                discovered_path=tmp / "discovered.json",
            )
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.bind(("127.0.0.1", 0))
            server.port = probe.getsockname()[1]
            probe.close()

            net_thread = threading.Thread(target=server._network_loop, daemon=True)
            net_thread.start()

            deadline = time.time() + 2.0
            conn: socket.socket | None = None
            while time.time() < deadline:
                try:
                    conn = socket.create_connection(("127.0.0.1", server.port), timeout=1.0)
                    break
                except OSError:
                    time.sleep(0.05)
            self.assertIsNotNone(conn, "server did not accept connections")
            assert conn is not None
            try:
                conn.settimeout(2.0)
                conn.sendall(FRAME.encode("utf-8"))
                ack = conn.recv(256).decode("utf-8", errors="replace")
                self.assertTrue(ack.startswith("ACK,sess-csv,3"), ack)
                conn.sendall(PULL.encode("utf-8"))
                ack = conn.recv(256).decode("utf-8", errors="replace")
                self.assertTrue(ack.startswith("ACK,sess-csv,0"), ack)
            finally:
                conn.close()

            time.sleep(0.2)
            server.stop()
            net_thread.join(timeout=2.0)
            server.logs.close()

            session_dirs = list((tmp / "logs" / "sessions").iterdir())
            self.assertEqual(len(session_dirs), 1, session_dirs)
            session_dir = session_dirs[0]
            self.assertTrue(session_dir.name.endswith("_sess-csv"), session_dir.name)

            with (session_dir / "telemetry.csv").open("r", encoding="utf-8", newline="") as f:
                reader = csv.DictReader(f)
                header = reader.fieldnames
                rows = list(reader)
            with (session_dir / "pulls.csv").open("r", encoding="utf-8", newline="") as f:
                pulls = list(csv.DictReader(f))
            self.assertTrue((session_dir / "session.json").exists())

        self.assertEqual(header, TELEMETRY_CSV_FIELDS,
                         "the CLI must write the shared v6 header, nothing else")
        self.assertEqual(len(rows), 1, f"expected one row, got {len(rows)}")
        row = rows[0]
        self.assertEqual(row["mode"], "RUN")
        self.assertEqual(row["phase"], "FLOAT")
        self.assertEqual(row["sample_3"], "-5.3")
        self.assertEqual(row["h5"], "0.5")
        self.assertEqual(row["r6"], "", "unmeasured resistance is an empty cell in v6")
        self.assertEqual(row["stepper0_position"], "100")
        self.assertEqual(row["stepper1_position"], "-50")
        self.assertEqual(row["stepper1_seq"], "flex")
        self.assertEqual(row["stepper0_zeroed"], "1")
        self.assertEqual(row["heaters_active"], "3")
        self.assertEqual(row["energy_wh"], "1.5")
        self.assertTrue(row["gs_rx_utc"].endswith("Z"))

        self.assertEqual(len(pulls), 1)
        self.assertEqual(pulls[0]["pull_id"], "7")
        self.assertEqual(pulls[0]["samples"], "4|5|6|7")

    # MUTATION: in telemetry_server._handle_connection replace
    # `self.logs.on_packet(packet, rx_utc)` with a no-op and confirm
    # test_server_writes_schema_v6_session_directory fails on the session
    # directory count (0 instead of 1).


if __name__ == "__main__":
    unittest.main()
