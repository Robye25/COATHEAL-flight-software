"""Alarm chips show a headline, the full text is the tooltip (the window must
never grow wider than the screen because a row of alarms did). No Qt."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.alarms import HEADLINE_MAX, headline  # noqa: E402


class HeadlineTests(unittest.TestCase):
    def test_name_before_the_dash(self) -> None:
        self.assertEqual(headline("OVERTEMP latched — heaters forced off until RESET_CTRL"), "OVERTEMP latched")
        self.assertEqual(headline("M1 STEP LOSS (2 events) — position uncertain; CHECK MOTOR1 says why. "
                                  "Check the mechanism, then SET ZERO (or STEPLOSS_ACK 1 from the console)"),
                         "M1 STEP LOSS (2 events)")
        self.assertEqual(headline("LINK STALE — no frame for 12 s"), "LINK STALE")

    def test_short_texts_stay_whole(self) -> None:
        self.assertEqual(headline("heaters inhibited (M0 moving)"), "heaters inhibited (M0 moving)")
        self.assertEqual(headline("onboard queue backlog 900 frames (draining)"),
                         "onboard queue backlog 900 frames (draining)")

    def test_long_texts_cut_at_a_clause_else_with_an_ellipsis(self) -> None:
        clause = "fallback plan RUNNING onboard: autonomous bend in progress, M0 then M1, no repeat"
        self.assertEqual(headline(clause), "fallback plan RUNNING onboard")
        no_clause = "heater energy 104.3 Wh is above 80 % of the budget of 130 Wh and the heaters stop there"
        self.assertEqual(headline(no_clause), no_clause[:HEADLINE_MAX - 1].rstrip() + "…")
        self.assertEqual(len(headline("x" * 80)), HEADLINE_MAX)


if __name__ == "__main__":
    unittest.main()
