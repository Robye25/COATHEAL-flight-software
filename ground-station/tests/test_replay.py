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
        self.assertTrue(flagged[-1], "the last queued frame still arrives at drain speed")
        # Caught up: live frames again, onboard time at 1x -> live within a few seconds.
        for k in range(12):
            v = c.classify(1444.0 + k, 1444.0 + k)
        self.assertFalse(v.is_replay, "live again once onboard time advances at 1x with no lag")
        self.assertLess(v.behind_s, 1.0, "only the sub-second residue of the drain's last frame")

    # MUTATION: make classify() return is_replay=False always and confirm
    # test_backlog_replay_is_flagged_until_it_catches_up fails on
    # "hours-old frames must be replay".

    def test_startup_replay_without_prior_live_frame(self) -> None:
        """The console starts in the middle of a drain (bench, 2026-08-28):
        every frame is newer than the last, so only the rate of onboard
        time gives it away -- 3 h of 1 Hz frames arriving at 8 per second."""
        c = ReplayClassifier()
        verdicts = []
        for k in range(200):
            verdicts.append(c.classify(onboard_ts=0.0 + k, rx=10800.0 + k / 8.0))
        self.assertTrue(verdicts[-1].is_replay, "8 onboard seconds per wall second is a replay")
        self.assertGreater(verdicts[-1].behind_s, 10000.0, "behind estimate falls back to the raw lag")
        self.assertIsNotNone(verdicts[-1].eta_s)
        self.assertAlmostEqual(verdicts[-1].eta_s, verdicts[-1].behind_s / 7.0, delta=verdicts[-1].behind_s * 0.05)
        self.assertTrue(any(v.is_replay for v in verdicts[:20]), "must be recognised within a few seconds")
        # The drain catches up: onboard time now advances at 1x.
        for k in range(30):
            v = c.classify(onboard_ts=10830.0 + k, rx=10830.0 + k)
        self.assertFalse(v.is_replay, "1x means live")

    # MUTATION: drop the `or fast` term from is_replay in classify() and
    # confirm test_startup_replay_without_prior_live_frame fails on
    # "8 onboard seconds per wall second is a replay".

    def test_high_tick_rate_live_is_not_replay(self) -> None:
        # 5 Hz live telemetry: five frames per second, onboard time still 1x.
        c = ReplayClassifier()
        for k in range(50):
            v = c.classify(onboard_ts=100.0 + k / 5.0, rx=200.0 + k / 5.0)
        self.assertFalse(v.is_replay)

    def test_missing_timestamp_is_never_replay(self) -> None:
        c = ReplayClassifier()
        self.assertFalse(c.classify(None, 5.0).is_replay)


class TaggedFrameTests(unittest.TestCase):
    """Live-first firmware stamps every frame with its age (`TX=`)."""

    def test_tagged_frames_are_classified_by_age_alone(self) -> None:
        c = ReplayClassifier()
        live = c.classify(1000.0, 5000.0, tx_age_s=0.0, queue_depth=2400)
        self.assertFalse(live.is_replay)
        self.assertTrue(live.tagged)
        self.assertEqual(live.backlog_frames, 2400)
        old = c.classify(1.0, 5000.1, tx_age_s=1800.0, queue_depth=2400)
        self.assertTrue(old.is_replay)
        self.assertAlmostEqual(old.behind_s, 1800.0)
        # Clocks hours apart are irrelevant to a stamped frame.
        self.assertFalse(c.classify(1.0, 5001.0, tx_age_s=1.0, queue_depth=2390).is_replay)

    # MUTATION: make classify() ignore tx_age_s (fall through to the clock
    # path) and confirm test_tagged_frames_are_classified_by_age_alone fails
    # on the last assertion (a 5000 s lag beyond the baseline reads as replay).

    def test_eta_from_queue_depth_slope(self) -> None:
        c = ReplayClassifier()
        v = None
        for k in range(6):
            v = c.classify(1000.0 + k, 5000.0 + k, tx_age_s=0.0, queue_depth=2400 - 9 * k)
        self.assertIsNotNone(v.eta_s)
        self.assertAlmostEqual(v.eta_s, (2400 - 45) / 9.0, delta=1.0)
        # Replay frames in between do not disturb the slope.
        c.classify(1.0, 5005.5, tx_age_s=3600.0, queue_depth=2400 - 45)
        self.assertAlmostEqual(c.classify(1006.0, 5006.0, tx_age_s=0.0, queue_depth=2400 - 54).eta_s,
                               (2400 - 54) / 9.0, delta=1.0)


if __name__ == "__main__":
    unittest.main()
