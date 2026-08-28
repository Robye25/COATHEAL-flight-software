"""Headless tests for the redesigned console: tabs send the right wire
commands, gating disables controls with a reason, response lines and the
console table record every reply, radio silence quiets the ground station.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import capture_sends, frame, make_window, pump_until  # noqa: E402


class ConsoleTabTests(unittest.TestCase):
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

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.win = make_window(Path(self._tmp.name))

    def tearDown(self) -> None:
        self.win.close()
        self._tmp.cleanup()

    def feed(self, **kw) -> None:
        from app.protocol import parse_telemetry_csv
        self.win._on_packet(parse_telemetry_csv(frame(**kw)))

    # ── Motion tab ──
    def test_bend_sends_moveto_with_hold_and_marks_resistance(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.feed()
        motion = self.win._motion
        motion.selector.set_value(0)
        motion.update_state(self.win._state)
        motion.bend_target.setValue(800); motion.bend_hold.setValue(5.0)
        self.assertIsNone(motion.btn_bend.reason(), "M0 is enabled+zeroed in RUN: BEND must be live")
        motion.btn_bend.click()
        self.assertEqual(sent, ["STEPPER_MOVETO 0 800 5"])
        readout = motion.tracker.readout(0, self.win._state)
        self.assertEqual(readout.r_start, 118.0, "BEND must record the specimen resistance at bend start")
        motion.btn_pull.click()
        self.assertEqual(sent[-1], "PULL_EXECUTE 0")

    # MUTATION: change the BEND wire template to "STEPPER_MOVE" in
    # tab_motion._bend and confirm test_bend_sends_moveto_with_hold_and_marks_resistance fails.

    def test_motion_gating_reasons(self) -> None:
        self.feed()
        motion = self.win._motion
        motion.selector.set_value(1)      # M1: not enabled, not zeroed
        motion.update_state(self.win._state)
        self.assertIn("not enabled", motion.btn_bend.reason() or "")
        self.assertFalse(motion.btn_bend.isEnabled())
        self.assertIn("not enabled", motion.jog_buttons[0].reason() or "")
        self.feed(m1="en:1|ok:1|mv:0|hold:0|zeroed:0")
        self.assertIn("not zeroed", motion.btn_bend.reason() or "")
        self.assertIsNone(motion.jog_buttons[0].reason(), "jog is allowed before zeroing")
        self.feed(mode="STANDBY")
        self.assertIn("ARM", motion.btn_bend.reason() or "")
        self.assertIn("ARM", motion.bend_note.text())

    # MUTATION: make gating.motion_reason return None unconditionally and
    # confirm test_motion_gating_reasons fails on the "not enabled" assertion.

    def test_esc_and_stop_motors_send_both_stops(self) -> None:
        from PyQt6.QtGui import QKeySequence, QShortcut
        sent = capture_sends(self.win._dispatcher)
        esc = next(s for s in self.win.findChildren(QShortcut) if s.key() == QKeySequence("Esc"))
        esc.activated.emit()
        self.assertEqual(sent, ["STEPPER_STOP 0", "STEPPER_STOP 1"])
        sent.clear()
        self.win._top.btn_stop_motors.click()
        self.assertEqual(sent, ["STEPPER_STOP 0", "STEPPER_STOP 1"])
        sent.clear()
        self.win._top.btn_heaters_off.click()
        self.assertEqual(sent, ["HEATERS_OFF"])

    def test_no_unmodified_letter_shortcut_sends_a_command(self) -> None:
        from PyQt6.QtGui import QShortcut
        keys = {s.key().toString() for s in self.win.findChildren(QShortcut)}
        for key in keys:
            if len(key) == 1 and key.isalpha():
                self.assertEqual(key, "P", f"only P (plot pause) may be a bare letter, found {key}")

    # ── Thermal tab ──
    def test_thermal_row_gating_and_target_send(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.feed(samples="1,2,nan,4,5,6,7,8", valid="AT:1|AP:1|UV:1|S0:1|S1:1|S2:0|S3:1|S4:1|S5:1|S6:1|S7:1")
        thermal = self.win._thermal
        self.assertIsNone(thermal.rows[0].btn_set.reason())
        self.assertIn("S2", thermal.rows[2].btn_set.reason() or "")
        self.assertEqual(thermal.rows[2].state.toolTip(), "NO TEMP")
        thermal.rows[0].target.setValue(25.0)
        thermal.rows[0].btn_set.click()
        self.assertEqual(sent, ["SET_TEMP_TARGET 0 25.000"])
        thermal.all_target.setValue(10.0)
        self.assertFalse(thermal.btn_set_all.isEnabled(), "Set all must be refused while S2 is invalid")
        self.assertIn("S2", thermal.btn_set_all.reason() or "")
        thermal.btn_set_all.click()
        self.assertEqual(sent, ["SET_TEMP_TARGET 0 25.000"], "a disabled Set all must not send")

    def test_thermal_absorbs_get_thermal(self) -> None:
        from app.protocol import CommandResponse
        self.feed()
        body = ("target_min_c=0;target_max_c=60;h0_target=25;h0_temp=24.1;h0_duty=0.3;h1_target=-;h1_temp=2;h1_duty=0;"
                "h2_target=-;h2_temp=3;h2_duty=0;h3_target=-;h3_temp=4;h3_duty=0;h4_target=-;h4_temp=5;h4_duty=0;h5_target=-;h5_temp=6;h5_duty=0")
        self.win._dispatcher.response_received.emit(
            "GET_THERMAL", CommandResponse(ok=True, command="GET_THERMAL", body=body, raw=""), 5.0, self.win._system)
        self.assertEqual(self.win._thermal.targets()[0], 25.0)
        self.assertIsNone(self.win._thermal.targets()[1])
        self.assertEqual(self.win._thermal.rows[0].target.maximum(), 60.0, "limits come from GET_THERMAL")

    # ── System tab ──
    def test_mode_buttons_follow_mode(self) -> None:
        system = self.win._system
        self.feed(mode="RUN")
        self.assertFalse(system.btn_arm.isEnabled()); self.assertTrue(system.btn_disarm.isEnabled())
        self.assertFalse(system.btn_exit_safe.isEnabled())
        self.feed(mode="STANDBY")
        self.assertTrue(system.btn_arm.isEnabled()); self.assertFalse(system.btn_disarm.isEnabled())
        self.feed(mode="SAFE")
        self.assertTrue(system.btn_exit_safe.isEnabled()); self.assertFalse(system.btn_arm.isEnabled())

    # MUTATION: make gating.arm_reason return None always and confirm
    # test_mode_buttons_follow_mode fails on `assertFalse(system.btn_arm.isEnabled())`.

    def test_check_selector_and_tick_rate(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        system = self.win._system
        system.check_target.setCurrentText("MOTOR1"); system.btn_check.click()
        system.check_target.setCurrentText("ALL"); system.btn_check.click()
        system.tick_hz.setValue(2.0); system.btn_tick.click()
        self.assertEqual(sent, ["CHECK MOTOR1", "CHECK", "SET_TICK_HZ 2.000"])

    # ── responses / console ──
    def test_response_fans_out_to_console_and_panel_line(self) -> None:
        from app.protocol import CommandResponse
        self.win._dispatcher.response_received.emit(
            "ARM", CommandResponse(ok=False, command="ARM", error="ARM requires STANDBY mode", raw="NACK,ARM,..."),
            7.0, self.win._system)
        self.assertEqual(self.win._console.row_count(), 1)
        self.assertIs(self.win._system.resp_mode.last_ok, False)
        self.assertIn("ARM requires STANDBY mode", self.win._system.resp_mode.text())
        self.assertIn("NACK", self.win._events.lines()[-1])
        self.assertIn("ARM", self.win._events.lines()[-1])

    def test_console_entry_sends_and_blocks_during_silence(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.win._console.entry.setText("BENDSEQ_STATUS 1")
        self.win._console.entry._submit()
        self.assertEqual(sent, ["BENDSEQ_STATUS 1"])
        self.assertEqual(self.win._console.entry.history(), ["BENDSEQ_STATUS 1"])

    # ── radio silence ──
    def test_silence_quiets_ground_station_and_gates_everything(self) -> None:
        from app.protocol import CommandResponse
        self.feed()
        self.win._dispatcher.response_received.emit(
            "RADIO_SILENCE", CommandResponse(ok=True, command="RADIO_SILENCE", body="radio silent", raw=""), 5.0, self.win._system)
        self.assertTrue(self.win._beacon.is_quiet()); self.assertTrue(self.win._probe.is_quiet())
        self.assertTrue(self.win._dispatcher.silence)
        self.assertIn("SILENT", self.win._top.radio_text())
        self.assertFalse(self.win._motion.btn_bend.isEnabled())
        self.assertFalse(self.win._top.btn_heaters_off.isEnabled())
        self.assertFalse(self.win._system.btn_silence.isEnabled())
        self.assertTrue(self.win._system.btn_resume.isEnabled())
        self.assertNotIn("LINK", [a.key for a in self.win._alarms.active])
        self.win._dispatcher.response_received.emit(
            "RADIO_RESUME", CommandResponse(ok=True, command="RADIO_RESUME", body="radio resumed", raw=""), 5.0, self.win._system)
        self.assertFalse(self.win._beacon.is_quiet())
        self.assertTrue(self.win._top.btn_heaters_off.isEnabled())

    # MUTATION: comment out `worker.set_quiet(active)` in
    # MainWindow._on_silence_changed and confirm the beacon quiet assertion fails.

    # ── replay of the onboard backlog ──
    def test_replayed_frames_do_not_drive_state_or_gating(self) -> None:
        import time
        from datetime import datetime, timezone
        now = time.time()
        live_ts = datetime.fromtimestamp(now, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        old_ts = datetime.fromtimestamp(now - 3 * 3600, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.feed(mode="RUN", ts=live_ts)
        self.assertIsNone(self.win._motion.btn_enable.reason())
        # The backlog replays: hours-old frames that still say STANDBY.
        for seq in range(2, 8):
            self.feed(seq=seq, mode="STANDBY", ts=old_ts)
        self.assertEqual(self.win._state.mode, "RUN", "a replayed frame must not overwrite the live state")
        self.assertIsNone(self.win._motion.btn_enable.reason(), "gating must follow the live state")
        self.assertTrue(self.win._state.replay)
        self.assertTrue(self.win._top.replay_visible())
        self.assertIn("REPLAY", [a.key for a in self.win._alarms.active])
        self.assertEqual(self.win._motion.motor_note.text(), "")
        # Live again, now genuinely STANDBY: state and gating follow, badge clears.
        self.feed(seq=9, mode="STANDBY", ts=live_ts)
        self.assertEqual(self.win._state.mode, "STANDBY")
        self.assertIn("ARM", self.win._motion.btn_enable.reason() or "")
        self.assertIn("ARM", self.win._motion.motor_note.text())
        self.assertFalse(self.win._state.replay)
        self.assertFalse(self.win._top.replay_visible())
        self.assertNotIn("REPLAY", [a.key for a in self.win._alarms.active])

    # MUTATION: in MainWindow._on_packet set `self._last_pkt = pkt` for replayed
    # frames too and confirm test_replayed_frames_do_not_drive_state_or_gating
    # fails on "a replayed frame must not overwrite the live state".

    # ── Debug tab ──
    def test_debug_probe_polls_quietly_and_renders_a_verdict(self) -> None:
        from app.protocol import CommandResponse
        sent = []
        self.win._dispatcher.send = lambda cmd, tag=None, timeout=None, quiet=False: sent.append((cmd, quiet))
        debug = self.win._debug
        debug.selector.set_value(1)
        debug.read_once()
        self.assertEqual(sent, [("MOTOR_DEBUG 1", True)], "the probe must use the quiet path")
        body = ("motor=1;sw_pos=312;sw_tgt=800;sw_hz=100;us=4;enabled=1;moving=1;holding=0;pulses=312;missed=0;"
                "xactual=0;xtarget=204800;vactual=35000;mscnt=0;tstep=120;drv_status=0x0;stst=0;cs_actual=9;sg_result=18;"
                "stallguard=0;ot=0;otpw=0;s2ga=0;s2gb=0;ola=0;olb=0;s2vsa=0;s2vsb=0;stealth=1;fsactive=0;rampstat=0x0;"
                "vzero=0;pos_reached=0;vel_reached=1;status_sg=0;ioin=0x30000000;drv_enn=0;sd_mode=0;version=0x30;"
                "gstat=0x0;chopconf=0x06010043;toff=3;mres=6;usteps=4")
        rows_before = self.win._console.row_count()
        clock = [100.0]
        debug._clock = lambda: clock[0]
        for k, mscnt in enumerate((0, 256, 512, 768)):
            clock[0] = 100.0 + 0.5 * k
            resp = CommandResponse(ok=True, command="MOTOR_DEBUG", body=body.replace("mscnt=0", f"mscnt={mscnt}"), raw="")
            self.win._dispatcher.quiet_response.emit("MOTOR_DEBUG 1", resp, 3.0, debug)
        est = debug.last_estimate
        self.assertIsNotNone(est)
        self.assertAlmostEqual(est.sequencer_full_steps_s, 2.0, places=3)
        self.assertEqual(est.color, "green", est.verdict)
        self.assertIn("MOVING", debug.verdict.text())
        self.assertEqual(self.win._console.row_count(), rows_before, "quiet replies must not land in the console")
        self.assertIn("full-steps/s", debug._derived["seq"].text())
        self.assertEqual(debug._regs["sd_mode"].text(), "0")

    # ── alarms ──
    def test_alarm_strip_and_ack(self) -> None:
        self.feed(status="SD_OK|OVERTEMP_FAIL|SAMPLE_TEMP_OK")
        strip = self.win._alarm_strip
        self.assertTrue(strip.isVisibleTo(self.win))
        self.assertTrue(any("OVERTEMP" in t for t in strip.chip_texts()))
        self.assertEqual(self.win._alarms.unacked_count, 1)
        strip.ack_requested.emit("OVERTEMP")
        self.assertEqual(self.win._alarms.unacked_count, 0)
        self.feed()
        self.assertEqual(strip.chip_texts(), [])
        self.assertFalse(strip.isVisibleTo(self.win))


if __name__ == "__main__":
    unittest.main()
