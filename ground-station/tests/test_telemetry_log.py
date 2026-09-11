"""Schema v6 row builder, appenders, and the session LogManager
(redesign spec §7). No Qt, no sockets."""
from __future__ import annotations

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import parse_pull_event, parse_telemetry_csv  # noqa: E402
from app.telemetry_log import (  # noqa: E402
    COMMAND_CSV_FIELDS, CTRL_COLUMNS, CsvAppender, LogManager, PULL_CSV_FIELDS,
    STEPPER_COLUMNS, TELEMETRY_CSV_FIELDS, TELEMETRY_SCHEMA_VERSION,
    packet_to_row, pull_to_row,
)

FRAME_A = (
    "DATA,coatheal-1787760547-1,10,2026-08-27T05:59:03Z,1,23.56,1009.16,0.01,"
    "-5.00,-5.10,nan,-5.30,-5.40,-5.50,-5.60,-5.70,"
    "HEATER_DUTY=0.00|0.10|0.20|0.30|0.40|0.50,"
    "RESISTANCE=118.42|-|-|-|121.05|-|-|-,"
    "PHASE=ASCENT,MODE=RUN,STATUS=SD_OK|LINK_OK,"
    "SENSOR_VALID=AT:1|AP:1|UV:0|S0:1|S1:1|S2:0|S3:1|S4:1|S5:1|S6:1|S7:1,"
    "SENSOR_AGE_MS=AT:12|AP:12|UV:-1|S0:5|S1:5|S2:-1|S3:5|S4:5|S5:5|S6:5|S7:5,"
    "COMPONENT_STATE=DPS310:OK|ADS1115:STALE|SEQUENT_RTD:DEGRADED|MOTOR0:FAILED|MOTOR1:OK|PWM:OK,"
    "CTRL=fallback:0|link_loss_s:0.0|energy_wh:12.4|budget_wh:130.0|budget_exhausted:0"
    "|heaters_active:3|queue:0|plan:none,"
    "STEPPER0=pos:0|tgt:0|hz:100|us:4|en:0|ok:0|mv:0|hold:0|hold_s:0|pulses:0|missed:0"
    "|src:cmd:ZERO|zeroed:0|seq:-|seqst:idle,"
    "STEPPER1=pos:312|tgt:800|hz:100|us:4|en:1|ok:1|mv:1|hold:0|hold_s:0|pulses:312|missed:2"
    "|src:cmd:BEND|zeroed:1|seq:-|seqst:idle"
)
FRAME_B = FRAME_A.replace("coatheal-1787760547-1,10,", "coatheal-1787760900-2,1,")
LEGACY = (
    "DATA,sess-legacy,1,2026-03-31T12:00:00Z,1,-30.00,150.00,1.23,"
    "-30.00,-30.10,-29.90,HEATER_DUTY=0.5|0.5|0.0,"
    "PHASE=ASCENT,STATUS=SD_OK"
)
PULL = "EVT,PULL,coatheal-1787760547-1,3,1,2026-08-27T05:59:07Z,800,5.00,4|5|6|7"


def _rows(path: Path) -> list:
    with path.open(encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


class SchemaTests(unittest.TestCase):
    def test_field_list_is_v6_and_unique(self) -> None:
        self.assertEqual(TELEMETRY_SCHEMA_VERSION, 6)
        self.assertEqual(len(TELEMETRY_CSV_FIELDS), len(set(TELEMETRY_CSV_FIELDS)))
        self.assertEqual(TELEMETRY_CSV_FIELDS[:3], ["gs_rx_utc", "session_id", "seq"])
        for column in ("sample_7", "h5", "r7", "stepper1_zeroed", "stepper1_seqstate",
                       "fallback", "energy_wh", "queue", "plan"):
            self.assertIn(column, TELEMETRY_CSV_FIELDS, column)
        self.assertEqual(TELEMETRY_CSV_FIELDS[-len(CTRL_COLUMNS):], CTRL_COLUMNS)

    # MUTATION: delete ("zeroed", "zeroed") from STEPPER_COLUMNS and confirm
    # test_field_list_is_v6_and_unique fails naming stepper1_zeroed.

    def test_row_is_lossless_and_complete(self) -> None:
        pkt = parse_telemetry_csv(FRAME_A)
        row = packet_to_row(pkt, rx_utc="2026-08-27T05:59:03.500Z")
        self.assertEqual(set(row), set(TELEMETRY_CSV_FIELDS), "every column, no extras")
        self.assertEqual(row["gs_rx_utc"], "2026-08-27T05:59:03.500Z")
        self.assertEqual(row["sample_2"], "nan")
        self.assertEqual(row["sample_3"], "-5.3")
        self.assertEqual(row["h1"], "0.1")
        self.assertEqual(row["r0"], "118.42")
        self.assertEqual(row["r1"], "", "unmeasured -> empty cell")
        self.assertEqual(row["sensor_valid"].split("|")[2], "UV:0")
        self.assertEqual(row["component_state"], "DPS310:OK|ADS1115:STALE|SEQUENT_RTD:DEGRADED|MOTOR0:FAILED|MOTOR1:OK|PWM:OK")
        self.assertEqual(row["stepper1_position"], "312")
        self.assertEqual(row["stepper1_ok"], "1")
        self.assertEqual(row["stepper1_missed"], "2")
        self.assertEqual(row["stepper1_zeroed"], "1")
        self.assertEqual(row["stepper0_zeroed"], "0")
        self.assertEqual(row["stepper1_source"], "cmd:BEND")
        self.assertEqual(row["energy_wh"], "12.4")
        self.assertEqual(row["heaters_active"], "3")
        self.assertEqual(row["plan"], "none")
        # Round trip through the CSV module preserves the numbers exactly.
        self.assertEqual(float(row["ambient_pressure_mbar"]), pkt.ambient_pressure_mbar)

    def test_legacy_frame_fills_blanks_not_errors(self) -> None:
        row = packet_to_row(parse_telemetry_csv(LEGACY), rx_utc="x")
        self.assertEqual(row["sample_3"], "")
        self.assertEqual(row["h3"], "")
        self.assertEqual(row["stepper0_position"], "")
        self.assertEqual(row["stepper1_zeroed"], "")
        self.assertEqual(row["fallback"], "")
        self.assertEqual(row["mode"], "")

    def test_pull_row(self) -> None:
        row = pull_to_row(parse_pull_event(PULL), rx_utc="x")
        self.assertEqual(list(row), PULL_CSV_FIELDS)
        self.assertEqual(row["samples"], "4|5|6|7")
        self.assertEqual(row["hold_s"], "5.0")


class CsvAppenderTests(unittest.TestCase):
    def test_header_once_and_append(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.csv"
            w = CsvAppender(path, ["a", "b"])
            w.write({"a": "1", "b": "2"})
            w.close()
            w2 = CsvAppender(path, ["a", "b"])
            w2.write({"a": "3", "b": "4"})
            w2.close()
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines, ["a,b", "1,2", "3,4"])

    def test_foreign_header_gets_a_sibling_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "telemetry.csv"
            path.write_text("old,schema\n1,2\n", encoding="utf-8")
            w = CsvAppender(path, ["a", "b"])
            self.assertEqual(w.path.name, "telemetry.1.csv",
                             "an older layout must never be appended to")
            w.write({"a": "1", "b": "2"})
            w.close()
            self.assertEqual(path.read_text(encoding="utf-8"), "old,schema\n1,2\n")
            self.assertTrue(w.path.exists())

    # MUTATION: make CsvAppender._compatible_path return `path` unconditionally
    # and confirm test_foreign_header_gets_a_sibling_file fails on the
    # sibling name assertion.


class LogManagerTests(unittest.TestCase):
    def test_session_switch_and_pending_flush(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "logs"
            mgr = LogManager(root, gs_info={"component": "test"})
            # Before any frame: commands/events are buffered, no directory.
            mgr.log_command("PING", True, 3.0, "pong", "ACK,PING,pong", ts_utc="t0")
            mgr.log_event("INFO", "before first frame", ts_utc="t0")
            self.assertIsNone(mgr.current_dir)
            self.assertFalse((root / "sessions").exists())

            opened = mgr.on_packet(parse_telemetry_csv(FRAME_A), rx_utc="rx1")
            self.assertTrue(opened, "first frame opens the session directory")
            dir_a = mgr.current_dir
            self.assertIsNotNone(dir_a)
            self.assertTrue(dir_a.name.endswith("_coatheal-1787760547-1"))
            self.assertFalse(mgr.on_packet(parse_telemetry_csv(FRAME_A), rx_utc="rx2"),
                             "same session must not report a new directory")
            mgr.on_pull(parse_pull_event(PULL), rx_utc="rx3")
            mgr.log_command("ARM", False, 9.0, "ARM requires STANDBY mode", "NACK,ARM,...", ts_utc="t1")

            # A new onboard session id (Pi restart) opens a second directory.
            self.assertTrue(mgr.on_packet(parse_telemetry_csv(FRAME_B), rx_utc="rx4"))
            dir_b = mgr.current_dir
            self.assertNotEqual(dir_a, dir_b)
            mgr.close()

            cmds_a = _rows(dir_a / "commands.csv")
            self.assertEqual([c["command"] for c in cmds_a], ["PING", "ARM"],
                             "the pre-session PING must be flushed into the first session")
            self.assertEqual(cmds_a[0]["ok"], "1")
            self.assertEqual(cmds_a[1]["ok"], "0")
            self.assertEqual(list(cmds_a[0]), COMMAND_CSV_FIELDS)
            events_a = (dir_a / "events.log").read_text(encoding="utf-8")
            self.assertIn("t0 INFO  before first frame", events_a)
            tele_a = _rows(dir_a / "telemetry.csv")
            self.assertEqual([r["seq"] for r in tele_a], ["10", "10"])
            pulls_a = _rows(dir_a / "pulls.csv")
            self.assertEqual(pulls_a[0]["pull_id"], "3")
            meta_a = json.loads((dir_a / "session.json").read_text(encoding="utf-8"))
            self.assertEqual(meta_a["frames"], 2)
            self.assertEqual(meta_a["pulls"], 1)
            self.assertEqual(meta_a["commands"], 2)
            self.assertEqual(meta_a["ground_station"]["component"], "test")
            self.assertIn("closed_utc", meta_a)
            tele_b = _rows(dir_b / "telemetry.csv")
            self.assertEqual([r["session_id"] for r in tele_b], ["coatheal-1787760900-2"])
            pointer = (root / "latest_session.txt").read_text(encoding="utf-8").strip()
            self.assertEqual(pointer, str(dir_b))

    # MUTATION: comment out `self._flush_pending(logs)` in LogManager._switch_to
    # and confirm test_session_switch_and_pending_flush fails: commands.csv of
    # the first session lists only ["ARM"].

    def test_session_meta_written_on_first_frame(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mgr = LogManager(Path(tmp) / "logs")
            mgr.on_packet(parse_telemetry_csv(FRAME_A), rx_utc="rx1")
            meta = json.loads((mgr.current_dir / "session.json").read_text(encoding="utf-8"))
            self.assertEqual(meta["first_frame_utc"], "rx1",
                             "session.json must be truthful even if the process dies before close()")
            self.assertEqual(meta["frames"], 1)
            mgr.close()

    # MUTATION: remove the `self._write_meta()` call from the first-frame
    # branch of SessionLogs.write_packet and confirm
    # test_session_meta_written_on_first_frame fails (first_frame_utc None).

    def test_close_without_session_keeps_operator_record(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "logs"
            mgr = LogManager(root)
            mgr.log_event("WARN", "onboard never answered", ts_utc="t0")
            mgr.close()
            dirs = list((root / "sessions").iterdir())
            self.assertEqual(len(dirs), 1)
            self.assertTrue(dirs[0].name.endswith("_no-session"))
            self.assertIn("onboard never answered", (dirs[0] / "events.log").read_text(encoding="utf-8"))

    def test_nothing_written_after_close(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mgr = LogManager(Path(tmp) / "logs")
            mgr.close()
            mgr.log_event("INFO", "late")
            self.assertFalse(mgr.on_packet(parse_telemetry_csv(FRAME_A)))
            self.assertFalse((Path(tmp) / "logs" / "sessions").exists())


class InterleavedSessionTests(unittest.TestCase):
    """The live-first drain interleaves the previous session's backlog with
    the current session's live frames: both stay open, commands go to the
    newest."""

    def test_two_sessions_stay_open_and_the_newest_is_current(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mgr = LogManager(root, gs_info={"gs": "test"})
            self.assertTrue(mgr.on_packet(parse_telemetry_csv(FRAME_B), rx_utc="rx1"))   # live (newer epoch)
            self.assertTrue(mgr.on_packet(parse_telemetry_csv(FRAME_A), rx_utc="rx2"))   # backlog of the previous session
            self.assertEqual(mgr.current_session_id, "coatheal-1787760900-2", "the newest session stays current")
            self.assertFalse(mgr.on_packet(parse_telemetry_csv(FRAME_A.replace(",10,", ",11,")), rx_utc="rx3"))
            self.assertFalse(mgr.on_packet(parse_telemetry_csv(FRAME_B.replace(",1,", ",2,")), rx_utc="rx4"))
            mgr.log_command("ARM", True, 1.0, body="mode=RUN")
            dir_a = mgr.dir_for("coatheal-1787760547-1")
            dir_b = mgr.dir_for("coatheal-1787760900-2")
            self.assertIsNotNone(dir_a); self.assertIsNotNone(dir_b)
            self.assertEqual(dir_b, mgr.current_dir)
            mgr.close()
            rows_a = (dir_a / "telemetry.csv").read_text(encoding="utf-8").splitlines()
            rows_b = (dir_b / "telemetry.csv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(rows_a), 3, "header + two backlog rows")
            self.assertEqual(len(rows_b), 3, "header + two live rows")
            self.assertIn("ARM", (dir_b / "commands.csv").read_text(encoding="utf-8"))
            self.assertTrue((dir_a / "session.json").exists() and (dir_b / "session.json").exists())

    # MUTATION: restore the single-session `_switch_to` behaviour (close A
    # when B opens) and confirm the test fails: FRAME_A's second row lands in
    # a re-opened directory / the command lands in session A.


if __name__ == "__main__":
    unittest.main()
