"""TIME_SYNC from the ground station (app/clock_sync.py): once per onboard
session, then every ten minutes, never waiting for the link budget; the
reply's offset becomes the status line. No Qt."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.clock_sync import (  # noqa: E402
    BUDGET_FULL_ERROR, FAILED_RETRY_S, RETRY_S, SYNC_PERIOD_S, ClockSync, format_age, format_offset,
    parse_reply,
)


class ScheduleTests(unittest.TestCase):
    def test_nothing_until_a_session_then_at_once(self) -> None:
        sync = ClockSync()
        self.assertFalse(sync.due(100.0, link_ok=True, silent=False), "no onboard session yet")
        sync.on_session("coatheal-1-1", 100.0)
        self.assertTrue(sync.due(100.0, link_ok=True, silent=False))
        self.assertFalse(sync.due(100.0, link_ok=False, silent=False), "no link, no exchange")
        self.assertFalse(sync.due(100.0, link_ok=True, silent=True), "radio silence sends nothing")

    def test_one_in_flight_at_a_time_then_every_period(self) -> None:
        sync = ClockSync()
        sync.on_session("s", 100.0)
        cmd = sync.command(1_760_000_000.123)
        self.assertEqual(cmd, "TIME_SYNC 1760000000123", "no round-trip hint before the first exchange")
        self.assertFalse(sync.due(100.5, True, False), "pending")
        line, level = sync.on_reply(True, "offset_ms=12;applied=0;rtt_ms=0;syncs=1;now=2026-10-10T12:00:00Z", 240.0, 101.0)
        self.assertEqual(level, "INFO")
        self.assertIn("in sync", line)
        self.assertFalse(sync.due(101.0 + SYNC_PERIOD_S - 1, True, False))
        self.assertTrue(sync.due(101.0 + SYNC_PERIOD_S, True, False))
        self.assertEqual(sync.command(1_760_000_600.0), "TIME_SYNC 1760000600000 120",
                         "half the last exchange latency is the round-trip hint")

    def test_a_new_session_is_synced_at_once(self) -> None:
        sync = ClockSync()
        sync.on_session("s1", 100.0)
        sync.command(1.0); sync.on_reply(True, "offset_ms=5;applied=0", 100.0, 101.0)
        self.assertFalse(sync.due(200.0, True, False))
        sync.on_session("s2", 200.0)   # the onboard rebooted: fake-hwclock time again
        self.assertTrue(sync.due(200.0, True, False))
        self.assertIsNone(sync.last_offset_ms, "the old offset says nothing about the new boot")

    def test_budget_full_is_postponed_not_counted(self) -> None:
        sync = ClockSync()
        sync.on_session("s", 100.0)
        sync.command(1.0)
        line, level = sync.on_reply(False, BUDGET_FULL_ERROR, 0.0, 101.0)
        self.assertEqual(level, "INFO")
        self.assertIn("postponed", line)
        self.assertEqual(sync.checks, 0)
        self.assertIsNone(sync.last_error, "a full budget is not a failure")
        self.assertFalse(sync.due(101.0 + RETRY_S - 1, True, False))
        self.assertTrue(sync.due(101.0 + RETRY_S, True, False))

    def test_a_refusal_is_a_failure_tried_again_later(self) -> None:
        sync = ClockSync()
        sync.on_session("s", 100.0)
        sync.command(1.0)
        line, level = sync.on_reply(False, "offset_ms=4200: cannot set the clock: Operation not permitted "
                                           "(the service needs AmbientCapabilities=CAP_SYS_TIME)", 150.0, 101.0)
        self.assertEqual(level, "WARN")
        self.assertIn("CAP_SYS_TIME", line)
        self.assertEqual(sync.status(102.0), ("sync failed: offset_ms=4200: cannot set the clock: Operation not "
                                              "permitted (the service needs AmbientCapabilities=CAP_SYS_TIME)", "red"))
        self.assertFalse(sync.due(101.0 + FAILED_RETRY_S - 1, True, False))
        self.assertTrue(sync.due(101.0 + FAILED_RETRY_S, True, False))
        # A timeout (no reply) is retried sooner.
        sync.command(1.0); sync.on_reply(False, "timed out", 3000.0, 500.0)
        self.assertTrue(sync.due(500.0 + RETRY_S, True, False))


class StatusTests(unittest.TestCase):
    def test_status_line_follows_the_last_reply(self) -> None:
        sync = ClockSync()
        self.assertEqual(sync.status(0.0), ("not checked", "muted"))
        sync.on_session("s", 100.0)
        sync.command(1.0)
        self.assertEqual(sync.status(100.0), ("checking…", "muted"))
        sync.on_reply(True, "offset_ms=-1340;applied=1;rtt_ms=0;syncs=1;now=x", 200.0, 101.0)
        self.assertEqual(sync.status(131.0), ("stepped -1.3 s · 30 s ago", "amber"))
        sync.command(2.0)
        sync.on_reply(True, "offset_ms=8;applied=0;rtt_ms=100;syncs=2;now=x", 200.0, 701.0)
        self.assertEqual(sync.status(701.0 + 600), ("in sync (+8 ms) · checked 10 min ago", "green"))

    def test_helpers(self) -> None:
        self.assertEqual(parse_reply("offset_ms=-5;applied=1;now=2026-10-10T12:00:00Z"),
                         {"offset_ms": "-5", "applied": "1", "now": "2026-10-10T12:00:00Z"})
        self.assertEqual(format_offset(999), "+999 ms")
        self.assertEqual(format_offset(-2500), "-2.5 s")
        self.assertEqual(format_age(45), "45 s")
        self.assertEqual(format_age(1800), "30 min")
        self.assertEqual(format_age(5400), "1.5 h")


if __name__ == "__main__":
    unittest.main()
