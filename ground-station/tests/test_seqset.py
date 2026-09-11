"""SeqSet: received-sequence bookkeeping for the out-of-order drain."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.seqset import SeqSet  # noqa: E402


class SeqSetTests(unittest.TestCase):
    def test_live_first_backlog_is_not_a_duplicate(self) -> None:
        s = SeqSet()
        self.assertTrue(s.add(2500))          # this tick's frame arrives first
        for seq in range(1, 2500):            # then the backlog, in order
            self.assertTrue(s.add(seq), seq)
        self.assertEqual(s.ranges(), [(1, 2500)], "adjacent ranges merge")
        self.assertFalse(s.add(1200))         # a genuine re-delivery
        self.assertEqual(len(s), 2500)
        self.assertEqual(s.max, 2500)

    # MUTATION: make add() return `seq > self.max` and confirm the test fails
    # on the first backlog frame.

    def test_merging_and_membership(self) -> None:
        s = SeqSet([(10, 12), (20, 22)])
        self.assertIn(11, s); self.assertNotIn(15, s); self.assertNotIn(9, s)
        s.add(13); s.add(19)
        self.assertEqual(s.ranges(), [(10, 13), (19, 22)])
        for seq in range(14, 19):
            s.add(seq)
        self.assertEqual(s.ranges(), [(10, 22)])
        s.add(5)
        self.assertEqual(s.ranges(), [(5, 5), (10, 22)])

    def test_json_round_trip_and_legacy_cursor(self) -> None:
        s = SeqSet([(3, 4), (9, 9)])
        self.assertEqual(SeqSet.from_json(s.to_json()).ranges(), [(3, 4), (9, 9)])
        legacy = SeqSet.from_json(1441)
        self.assertIn(0, legacy); self.assertIn(1441, legacy); self.assertNotIn(1442, legacy)
        self.assertEqual(SeqSet.from_json(-1).ranges(), [])
        self.assertEqual(SeqSet.from_json(None).ranges(), [])
        self.assertEqual(SeqSet.from_json([[1, "x"], [5, 6]]).ranges(), [(5, 6)])


if __name__ == "__main__":
    unittest.main()
