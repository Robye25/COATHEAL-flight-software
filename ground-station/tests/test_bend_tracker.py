"""Resistance delta tracking across a bend (redesign spec §5.4)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.bend_tracker import BendTracker  # noqa: E402
from app.gui.state import state_from_packet  # noqa: E402
from app.protocol import parse_telemetry_csv  # noqa: E402


def st(r4: str, moving: bool):
    line = (f"DATA,s,1,2026-08-28T00:00:00Z,1,20,1000,0.1,1,2,3,4,5,6,7,8,"
            f"HEATER_DUTY=0|0|0|0|0|0,RESISTANCE=118.0|-|-|-|{r4}|-|-|-,PHASE=ASCENT,MODE=RUN,STATUS=SD_OK,"
            f"STEPPER0=pos:0|tgt:0|hz:100|us:4|en:1|mv:0|hold:0|hold_s:0|pulses:0|src:init,"
            f"STEPPER1=pos:0|tgt:800|hz:100|us:4|en:1|mv:{1 if moving else 0}|hold:0|hold_s:0|pulses:0|src:cmd:BEND")
    return state_from_packet(parse_telemetry_csv(line))


class BendTrackerTests(unittest.TestCase):
    def test_operator_mark_then_delta(self) -> None:
        tracker = BendTracker()
        tracker.mark_start(1, st("120.0", False), "t0")
        tracker.update(st("120.0", True), "t1")     # motion starts: must keep the operator mark
        readout = tracker.readout(1, st("117.6", True))
        self.assertEqual(readout.sample, 4)
        self.assertEqual(readout.r_start, 120.0)
        self.assertEqual(readout.r_now, 117.6)
        self.assertAlmostEqual(readout.delta_pct, -2.0)
        self.assertEqual(readout.started_utc, "t0")
        self.assertEqual(readout.source, "operator")

    # MUTATION: in BendTracker.update, always call mark_start on the
    # idle->moving edge (drop the "operator" check) and confirm
    # test_operator_mark_then_delta fails: r_start becomes 120.0 at "t1"
    # instead of the operator's "t0" (started_utc assertion).

    def test_auto_mark_on_motion_edge(self) -> None:
        tracker = BendTracker()
        tracker.update(st("121.0", False), "t0")
        tracker.update(st("121.0", True), "t1")
        readout = tracker.readout(1, st("130.0", True))
        self.assertEqual(readout.r_start, 121.0)
        self.assertEqual(readout.started_utc, "t1")
        self.assertEqual(readout.source, "motion")
        self.assertAlmostEqual(readout.delta_pct, (130.0 - 121.0) / 121.0 * 100.0)

    def test_unmonitored_motor_reports_nothing(self) -> None:
        tracker = BendTracker()
        line_no_r = st("-", False)
        readout = tracker.readout(1, line_no_r)
        self.assertIsNone(readout.sample)
        self.assertIsNone(readout.delta_pct)
        m0 = tracker.readout(0, st("120.0", False))
        self.assertEqual(m0.sample, 0)
        self.assertIsNone(m0.delta_pct, "no bend marked yet -> no delta")


if __name__ == "__main__":
    unittest.main()
