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
        motion.bend_target.setValue(2.0); motion.bend_hold.setValue(5.0)
        self.assertIsNone(motion.btn_bend.reason(), "M0 is enabled+zeroed in RUN: BEND must be live")
        motion.btn_bend.click()
        self.assertEqual(sent, ["STEPPER_MOVETO_MM 0 2.000 5"])
        readout = motion.tracker.readout(0, self.win._state)
        self.assertEqual(readout.r_start, 118.0, "BEND must record the specimen resistance at bend start")
        motion.btn_pull.click()
        self.assertEqual(sent[-1], "PULL_EXECUTE 0")

    # MUTATION: change the BEND wire template to "STEPPER_MOVE_MM" in
    # tab_motion._bend and confirm test_bend_sends_moveto_with_hold_and_marks_resistance fails.

    def test_jog_and_drive_settings_send_mm_and_drive_commands(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.feed()
        motion = self.win._motion
        motion.selector.set_value(0)
        motion.update_state(self.win._state)
        motion.jog_buttons[3].click()   # +0.1 mm
        self.assertEqual(sent[-1], "STEPPER_MOVE_MM 0 0.100")
        motion.current.setValue(0.4)
        motion.btn_current.click()
        self.assertEqual(sent[-1], "STEPPER_SET_CURRENT 0 0.400")
        motion.accel.setValue(4.0)          # mm/s², sent as full-steps/s² (200 per mm at the 1 mm lead)
        motion.btn_accel.click()
        self.assertEqual(sent[-1], "STEPPER_SET_ACCEL 0 800.0")
        motion.speed.setValue(0.25)         # mm/s, sent as full-steps/s
        motion.btn_speed.click()
        self.assertEqual(sent[-1], "STEPPER_SET_SPEED 0 50.000")

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

    def test_thermal_targets_follow_external_clears(self) -> None:
        from app.protocol import CommandResponse
        self.feed()
        thermal = self.win._thermal
        disp = self.win._dispatcher
        ok = lambda verb, body="ok": CommandResponse(ok=True, command=verb, body=body, raw="")  # noqa: E731
        disp.response_received.emit("SET_ALL_TEMP_TARGETS 30", ok("SET_ALL_TEMP_TARGETS"), 5.0, self.win._system)
        self.assertEqual(thermal.targets(), [30.0] * 6, "targets set from another tab must be mirrored")
        # The panic button lives on the top panel: its ACK clears every
        # target onboard (protocol.md), so no row may keep saying PID.
        disp.response_received.emit("HEATERS_OFF", ok("HEATERS_OFF", "all heaters disabled"), 5.0, self.win._top)
        self.assertEqual(thermal.targets(), [None] * 6)
        disp.response_received.emit("SET_TEMP_TARGET 2 25", ok("SET_TEMP_TARGET"), 5.0, thermal)
        self.assertEqual(thermal.targets()[2], 25.0)
        disp.response_received.emit("SET_HEATER_DUTY 2 0.100", ok("SET_HEATER_DUTY"), 5.0, self.win._system)
        self.assertIsNone(thermal.targets()[2], "a duty override clears that channel's target onboard")
        # A NACK changes nothing.
        disp.response_received.emit("SET_ALL_TEMP_TARGETS 40",
                                    CommandResponse(ok=False, command="SET_ALL_TEMP_TARGETS", error="RUN mode required", raw=""),
                                    5.0, thermal)
        self.assertEqual(thermal.targets(), [None] * 6)

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

    def test_tagged_live_first_drain_keeps_panels_live(self) -> None:
        from app.protocol import parse_telemetry_csv
        ctrl = "fallback:0|link_loss_s:0.0|energy_wh:1.0|budget_wh:130.0|budget_exhausted:0|heaters_active:0|queue:{q}|plan:none"
        # Live-first firmware: this tick's frame (TX=0) arrives before the
        # previous session's backlog (TX=1800, a different session id).
        self.win._on_packet(parse_telemetry_csv(frame(seq=3000, mode="RUN", ctrl=ctrl.format(q=2400)) + ",TX=0"))
        self.assertEqual(self.win._state.mode, "RUN")
        for k in range(5):
            self.win._on_packet(parse_telemetry_csv(
                frame(seq=100 + k, session="coatheal-1787700000-9", mode="STANDBY",
                      ts="2026-08-27T00:00:00Z", ctrl=ctrl.format(q=2400)) + ",TX=1800"))
            self.win._on_packet(parse_telemetry_csv(frame(seq=3001 + k, mode="RUN", ctrl=ctrl.format(q=2395 - 5 * k)) + ",TX=0"))
        self.assertEqual(self.win._state.mode, "RUN", "old-session backlog frames must not touch the panels")
        self.assertIsNone(self.win._motion.btn_enable.reason())
        self.assertTrue(self.win._state.replay)
        self.assertTrue(self.win._state.replay_live_panels)
        self.assertEqual(self.win._state.replay_backlog_frames, 2375)
        self.assertTrue(self.win._top.replay_visible())
        texts = {a.key: a.text for a in self.win._alarms.active}
        self.assertIn("panels are LIVE", texts.get("REPLAY", ""))
        self.assertNotIn("RX_QUEUE", texts, "one alarm for the backlog, not two")
        # The queue empties: replay condition clears once no replay frame has
        # arrived for a while and the reported depth is back to normal.
        self.win._last_replay_mono -= 10.0
        self.win._on_packet(parse_telemetry_csv(frame(seq=3010, mode="RUN", ctrl=ctrl.format(q=0)) + ",TX=0"))
        self.assertFalse(self.win._state.replay)
        self.assertFalse(self.win._top.replay_visible())

    # MUTATION: in _on_packet, ignore pkt.tx_age_s when calling classify() and
    # confirm test_tagged_live_first_drain_keeps_panels_live fails: the
    # untagged path takes the first 2026-08-27 frame as a clock baseline.

    def test_ack_mode_is_applied_before_the_next_live_frame(self) -> None:
        import time
        from datetime import datetime, timezone
        from app.protocol import CommandResponse
        now = time.time()
        live_ts = datetime.fromtimestamp(now, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        old_ts = datetime.fromtimestamp(now - 3 * 3600, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.feed(mode="STANDBY", ts=live_ts)
        self.assertIn("ARM", self.win._motion.btn_enable.reason() or "")
        self.win._on_response("ARM", CommandResponse(ok=True, command="ARM", body="mode=RUN;manual_control=1", raw=""), 1.0, None)
        self.assertEqual(self.win._state.mode, "RUN", "the ACK is the onboard's word on its mode")
        self.assertIsNone(self.win._motion.btn_enable.reason())
        self.assertIn("now RUN", self.win._system.btn_arm.reason() or "")
        # A replayed STANDBY frame (untagged firmware) must not undo it...
        for seq in range(2, 8):
            self.feed(seq=seq, mode="STANDBY", ts=old_ts)
        self.assertEqual(self.win._state.mode, "RUN")
        # ...and the next live frame is authoritative again.
        self.feed(seq=9, mode="STANDBY", ts=live_ts)
        self.assertEqual(self.win._state.mode, "STANDBY")

    # MUTATION: delete the `mode=` handling in _on_response and confirm
    # test_ack_mode_is_applied_before_the_next_live_frame fails on "the ACK is
    # the onboard's word on its mode".

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
        self.assertIn("full-steps/s", debug.rate.text())
        self.assertEqual(debug._regs["sd_mode"].text(), "0")
        # Driver health line: thermal state leads (ot=0/otpw=0 in the body).
        self.assertIn("die < 120", debug.health.text())

    # ── PID autotune ──
    def test_pid_autotune_start_and_result_flow(self) -> None:
        from app.protocol import CommandResponse
        sent = []
        self.win._dispatcher.send = lambda cmd, tag=None, timeout=None, quiet=False: (
            cmd != "GET_LAYOUT" and sent.append((cmd, quiet)))
        thermal = self.win._thermal
        self.feed()
        thermal.tune_heater.setCurrentIndex(4)
        thermal.tune_setpoint.setValue(40.0)
        thermal._tune_start()
        self.assertEqual(sent[0], ("PID_TUNE_START 4 40 0.5 4", False))
        self.assertEqual(sent[1], ("PID_TUNE_STATUS", True), "status poll uses the quiet path")
        done = CommandResponse(ok=True, command="PID_TUNE_STATUS",
                               body="state=done;heater=4;setpoint_c=40;relay_duty=0.5;hysteresis_c=1;"
                                    "cycles=4/4;relay=off;elapsed_s=300;ku=0.35;tu_s=14.2;amplitude_c=1.1;"
                                    "kp=0.109;ki=0.0035;kd=0.246;zn_kp=0.21;zn_ki=0.03;zn_kd=0.37", raw="")
        self.win._dispatcher.quiet_response.emit("PID_TUNE_STATUS", done, 3.0, thermal)
        self.assertIn("kp=0.109", thermal.tune_status.text().replace("\u200b", ""))
        self.assertIsNone(thermal.btn_tune_apply.reason(), "APPLY unlocks once a result exists")
        thermal._tune_apply()
        self.assertEqual(sent[-1][0], "SET_PID 4 0.109 0.0035 0.246")

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
