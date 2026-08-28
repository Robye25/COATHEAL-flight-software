"""Replay-vs-live classification (redesign follow-up, 2026-08-28)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.replay import ReplayClassifier, parse_onboard_timestamp  # noqa: E402


class TimestampTests(unittest.TestCase):
    def test_parse(self) -> None:
        self.assertEqual(parse_onboard_timestamp("1970-01-01T00:00:10Z"), 10.0)
        self.assertEqual(parse_onboard_timestamp("1970-01-01T00:00:10.500Z"), 10.5)
        self.assertIsNone(parse_onboard_timestamp("garbage"))
        self.assertIsNone(parse_onboard_timestamp(""))


class ClassifierTests(unittest.TestCase):
    def test_live_frames_with_clock_offset_are_live(self) -> None:
        c = ReplayClassifier()
        # Ground clock 90 s ahead of the onboard clock: still live.
        for i in range(10):
            v = c.classify(onboard_ts=1000.0 + i, rx=1090.0 + i)
            self.assertFalse(v.is_replay, i)
        self.assertAlmostEqual(c.clock_offset_s, 90.0)

    def test_backlog_replay_is_flagged_until_it_catches_up(self) -> None:
        c = ReplayClassifier()
        # Live first, then an outage, then the backlog replays at 10 frames/s.
        for i in range(5):
            self.assertFalse(c.classify(1000.0 + i, 1000.0 + i).is_replay)
        # The outage lasted 395 s; the queued frames (onboard 1005..1444)
        # arrive at 10/s from rx 1400 and catch up with the present at ~1444.
        flagged = []
        for k in range(440):
            v = c.classify(1005.0 + k, 1400.0 + k * 0.1)
            flagged.append(v.is_replay)
            if k == 50:
                self.assertGreater(v.behind_s, 300.0)
                self.assertIsNotNone(v.eta_s)
                self.assertGreater(v.eta_s, 0.0)
        self.assertTrue(all(flagged[:250]), "hours-old frames must be replay")
        self.assertFalse(flagged[-1], "the tail of the backlog is within the threshold and live again")

    # MUTATION: make classify() return is_replay=False always and confirm
    # test_backlog_replay_is_flagged_until_it_catches_up fails on
    # "hours-old frames must be replay".

    def test_startup_replay_without_prior_live_frame(self) -> None:
        c = ReplayClassifier()
        # GS starts mid-drain: frames from 3 h ago arriving now, getting newer.
        v = c.classify(onboard_ts=0.0, rx=10800.0)
        self.assertFalse(v.is_replay, "the first frame defines the best lag so far")
        v = c.classify(onboard_ts=10.0, rx=10801.0)
        self.assertFalse(v.is_replay, "still the same lag band")
        # Once newer frames show a much smaller lag, the old band is exposed:
        # simulate the drain reaching the present.
        v = c.classify(onboard_ts=10790.0, rx=10802.0)
        self.assertFalse(v.is_replay)
        v_old = c.classify(onboard_ts=100.0, rx=10803.0)
        self.assertTrue(v_old.is_replay, "an old frame after a live one is a replay")

    def test_missing_timestamp_is_never_replay(self) -> None:
        c = ReplayClassifier()
        self.assertFalse(c.classify(None, 5.0).is_replay)


if __name__ == "__main__":
    unittest.main()
