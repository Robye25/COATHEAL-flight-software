"""Log sessions started by the operator (LogManager.start_new_session): the
open files close, the next frame opens `<now>_<session id>_manual`, and what
happens in between is buffered into it. Session display names carry the
date, the time and the id. No Qt."""
from __future__ import annotations

import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import frame  # noqa: E402
from app.protocol import parse_telemetry_csv  # noqa: E402
from app.session_dir import session_dir_name, session_display  # noqa: E402
from app.telemetry_log import LogManager  # noqa: E402


class SessionNameTests(unittest.TestCase):
    def test_display_name_is_date_time_and_id(self) -> None:
        stamp = datetime.fromtimestamp(1787760547, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
        self.assertEqual(session_display("coatheal-1787760547-1"), f"{stamp} · 1787760547-1")
        self.assertEqual(session_display("sess-smoke"), "sess-smoke", "no epoch in the id: the id itself")
        self.assertEqual(session_display(""), "—")

    def test_manual_directory_is_stamped_now_not_at_boot(self) -> None:
        automatic = session_dir_name("coatheal-1787760547-1")
        self.assertTrue(automatic.endswith("_coatheal-1787760547-1"))
        manual = session_dir_name("coatheal-1787760547-1", now=1760000000.0, suffix="manual")
        stamp = datetime.fromtimestamp(1760000000.0, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
        self.assertEqual(manual, f"{stamp}_coatheal-1787760547-1_manual")


class ManualSessionTests(unittest.TestCase):
    def test_new_session_rotates_the_files_and_keeps_the_buffered_commands(self) -> None:
        pkt = parse_telemetry_csv(frame())
        with tempfile.TemporaryDirectory() as tmp:
            mgr = LogManager(Path(tmp))
            self.assertTrue(mgr.on_packet(pkt))
            first = mgr.current_dir
            self.assertIsNotNone(first)
            mgr.log_command("STATUS", True, 12.0, "ok", "ACK,STATUS,ok")
            mgr.start_new_session(now=1760000000.0, session_id=pkt.session_id)
            self.assertIsNone(mgr.current_dir, "no folder until the next frame opens it")
            mgr.log_command("PING", True, 3.0, "pong", "ACK,PING,pong")
            self.assertTrue(mgr.on_packet(pkt), "the next frame opens a new session folder")
            second = mgr.current_dir
            self.assertNotEqual(second, first)
            self.assertTrue(second.name.endswith("_manual"), second.name)
            self.assertTrue(second.name.startswith(
                datetime.fromtimestamp(1760000000.0, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")))
            # The same onboard session keeps writing into the manual folder.
            self.assertFalse(mgr.on_packet(pkt))
            self.assertEqual(mgr.current_dir, second)
            mgr.close()
            first_commands = (first / "commands.csv").read_text(encoding="utf-8")
            second_commands = (second / "commands.csv").read_text(encoding="utf-8")
            self.assertIn("STATUS", first_commands); self.assertNotIn("PING", first_commands)
            self.assertIn("PING", second_commands); self.assertNotIn("STATUS", second_commands)
            self.assertEqual((second / "telemetry.csv").read_text(encoding="utf-8").count("\n"), 3,
                             "header + two frames in the manual session")

    def test_a_manual_session_for_an_unknown_session_takes_the_first_one_seen(self) -> None:
        pkt = parse_telemetry_csv(frame())
        with tempfile.TemporaryDirectory() as tmp:
            mgr = LogManager(Path(tmp))
            mgr.start_new_session(now=1760000000.0, session_id=None)
            self.assertTrue(mgr.on_packet(pkt))
            self.assertTrue(mgr.current_dir.name.endswith("_manual"))
            mgr.close()


if __name__ == "__main__":
    unittest.main()
