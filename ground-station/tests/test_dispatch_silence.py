"""Radio-silence gate in the command dispatcher and the discovery workers
(redesign spec §9), plus the dispatcher's command log. Headless: the
dispatcher's thread pool is replaced by a recorder so no socket is touched."""
from __future__ import annotations

import csv
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import CommandResponse, parse_telemetry_csv  # noqa: E402
from app.telemetry_log import LogManager  # noqa: E402


class _FakePool:
    def __init__(self) -> None:
        self.jobs: list = []

    def start(self, job, priority: int = 0) -> None:
        self.jobs.append(job)


class DispatcherSilenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as exc:
            raise unittest.SkipTest(f"no Qt platform: {exc}")

    def _dispatcher(self, log_manager=None):
        from app.gui.dispatch import CommandDispatcher
        d = CommandDispatcher("127.0.0.1", 5000, log_manager=log_manager)
        pool = _FakePool()
        d._pool = pool
        responses: list = []
        d.response_received.connect(lambda cmd, resp, ms, tag: responses.append((cmd, resp, ms, tag)))
        return d, pool, responses

    def test_silence_blocks_everything_but_the_whitelist(self) -> None:
        from app.gui.dispatch import SILENCE_BLOCK_ERROR
        d, pool, responses = self._dispatcher()
        d.set_silence(True)
        d.send("STEPPER_MOVE 0 100", tag="btn")
        self.assertEqual(pool.jobs, [], "a blocked command must never reach the network")
        self.assertEqual(len(responses), 1)
        cmd, resp, ms, tag = responses[0]
        self.assertEqual(cmd, "STEPPER_MOVE 0 100")
        self.assertFalse(resp.ok)
        self.assertEqual(resp.error, SILENCE_BLOCK_ERROR)
        self.assertEqual(tag, "btn")
        for allowed in ("RADIO_RESUME", "STATUS", "PING", "RADIO_SILENCE"):
            d.send(allowed)
        self.assertEqual([j._cmd for j in pool.jobs], ["RADIO_RESUME", "STATUS", "PING", "RADIO_SILENCE"])
        self.assertEqual(d.blocked_reason("HEATERS_OFF"), SILENCE_BLOCK_ERROR)
        self.assertIsNone(d.blocked_reason("radio_resume"))

    # MUTATION: make CommandDispatcher.blocked_reason always return None and
    # confirm test_silence_blocks_everything_but_the_whitelist fails on
    # `pool.jobs == []` (STEPPER_MOVE is queued).

    def test_not_silent_sends_everything(self) -> None:
        d, pool, responses = self._dispatcher()
        d.send("STEPPER_MOVE 0 100")
        self.assertEqual([j._cmd for j in pool.jobs], ["STEPPER_MOVE 0 100"])
        self.assertEqual(responses, [])

    def test_silence_state_follows_acks_and_status(self) -> None:
        d, pool, responses = self._dispatcher()
        changes: list = []
        d.silence_changed.connect(changes.append)
        ok = lambda cmd, body="": CommandResponse(ok=True, command=cmd, body=body, raw=f"ACK,{cmd},{body}")
        d.response_received.emit("RADIO_SILENCE", ok("RADIO_SILENCE", "radio silent"), 5.0, None)
        self.assertTrue(d.silence)
        d.response_received.emit("RADIO_RESUME", ok("RADIO_RESUME", "radio resumed"), 5.0, None)
        self.assertFalse(d.silence)
        d.response_received.emit("STATUS", ok("STATUS", "phase=ASCENT;mode=RUN;tick_hz=1;silence=1"), 5.0, None)
        self.assertTrue(d.silence, "a STATUS reply reporting silence=1 must engage the gate (GS restarted mid-silence)")
        d.response_received.emit("STATUS", ok("STATUS", "phase=ASCENT;silence=0"), 5.0, None)
        self.assertFalse(d.silence)
        # A NACKed RADIO_SILENCE must not engage the gate.
        d.response_received.emit("RADIO_SILENCE", CommandResponse(ok=False, command="RADIO_SILENCE", error="x"), 5.0, None)
        self.assertFalse(d.silence)
        self.assertEqual(changes, [True, False, True, False])

    # MUTATION: delete the `elif verb == "STATUS":` branch in _track_silence
    # and confirm test_silence_state_follows_acks_and_status fails on the
    # "STATUS reply reporting silence=1" assertion.

    def test_every_response_is_logged_including_local_refusals(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mgr = LogManager(Path(tmp) / "logs")
            d, pool, responses = self._dispatcher(log_manager=mgr)
            d.response_received.emit("PING", CommandResponse(ok=True, command="PING", body="pong", raw="ACK,PING,pong"), 4.2, None)
            d.set_silence(True)
            d.send("HEATERS_OFF")
            frame = ("DATA,coatheal-1787760547-9,1,2026-08-27T05:59:03Z,1,1,1,1,1,1,1,1,1,1,1,1,"
                     "HEATER_DUTY=0|0|0|0|0|0,PHASE=ASCENT,STATUS=SD_OK")
            mgr.on_packet(parse_telemetry_csv(frame), rx_utc="rx")
            mgr.close()
            session_dir = next((Path(tmp) / "logs" / "sessions").iterdir())
            with (session_dir / "commands.csv").open(encoding="utf-8", newline="") as fh:
                rows = list(csv.DictReader(fh))
            self.assertEqual([(r["command"], r["ok"]) for r in rows], [("PING", "1"), ("HEATERS_OFF", "0")])
            self.assertIn("radio silence", rows[1]["body"])
            self.assertEqual(rows[0]["latency_ms"], "4.2")


class DiscoveryQuietTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        cls._app = QApplication.instance() or QApplication([])

    def test_quiet_flags_are_settable_before_start(self) -> None:
        from app.gui.discovery import CommandProbe, GsBeacon
        beacon = GsBeacon(tel_port=4000, cmd_port=5000)
        probe = CommandProbe(["127.0.0.1"], cmd_port=5000, include_static=False)
        self.assertFalse(beacon.is_quiet())
        self.assertFalse(probe.is_quiet())
        beacon.set_quiet(True)
        probe.set_quiet(True)
        self.assertTrue(beacon.is_quiet())
        self.assertTrue(probe.is_quiet())
        beacon.set_quiet(False)
        self.assertFalse(beacon.is_quiet())

    def test_quiet_probe_sends_nothing(self) -> None:
        import time
        from app.gui.discovery import CommandProbe
        from app.link_budget import GROUND_SHARE, LinkBudget
        probe = CommandProbe(["127.0.0.1"], cmd_port=1, interval_s=0.05, timeout_s=0.05,
                             include_static=False, budget=LinkBudget(GROUND_SHARE))
        probe.set_quiet(True)
        probe.start()
        time.sleep(0.3)
        probe.stop()
        probe.wait(2000)
        self.assertEqual(probe.probes_sent, 0, "a quiet probe must not open a single connection")

    # MUTATION: remove the `if self._quiet.is_set(): ... continue` block from
    # CommandProbe.run and confirm test_quiet_probe_sends_nothing fails
    # (probes_sent > 0 within 0.3 s at a 0.05 s interval).

    def test_active_probe_does_send(self) -> None:
        import time
        from app.gui.discovery import CommandProbe
        from app.link_budget import GROUND_SHARE, LinkBudget
        # Its own ledger: the process-wide one may still hold another test's
        # exchanges.
        probe = CommandProbe(["127.0.0.1"], cmd_port=1, interval_s=0.05, timeout_s=0.05,
                             include_static=False, budget=LinkBudget(GROUND_SHARE))
        probe.start()
        time.sleep(0.3)
        probe.stop()
        probe.wait(2000)
        self.assertGreater(probe.probes_sent, 0)


if __name__ == "__main__":
    unittest.main()
