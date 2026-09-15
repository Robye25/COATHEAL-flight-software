"""OnboardState snapshot + gating reasons (redesign spec §6.2). No Qt."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui import gating  # noqa: E402
from app.gui.state import OnboardState, state_from_packet  # noqa: E402
from app.protocol import parse_telemetry_csv  # noqa: E402


def frame(*, mode="RUN", ctrl="fallback:0", m0="en:1|zeroed:1", m1="en:0|zeroed:0",
          samples="1,2,nan,4,5,6,7,8", valid="AT:1|AP:1|UV:1|S0:1|S1:1|S2:0|S3:1|S4:1|S5:1|S6:1|S7:1",
          status="SD_OK|USB_OK|LINK_OK|HEATER_ACTIVE") -> str:
    return (f"DATA,coatheal-1-1,1,2026-08-28T00:00:00Z,1,20,1000,0.1,{samples},"
            f"HEATER_DUTY=0.1|0|0|0|0|0,RESISTANCE=118|-|-|-|121|-|-|-,PHASE=ASCENT,MODE={mode},"
            f"STATUS={status},SENSOR_VALID={valid},COMPONENT_STATE=MOTOR0:OK|MOTOR1:FAILED,CTRL={ctrl},"
            f"STEPPER0=pos:1|tgt:2|hz:100|us:4|ok:1|mv:0|hold:0|hold_s:0|pulses:0|src:init|{m0},"
            f"STEPPER1=pos:0|tgt:0|hz:100|us:4|ok:0|mv:1|hold:0|hold_s:0|pulses:0|src:init|{m1}")


def state(**kw) -> OnboardState:
    silence = kw.pop("silence", False)
    return state_from_packet(parse_telemetry_csv(frame(**kw)), silence=silence)


class StateTests(unittest.TestCase):
    def test_snapshot_fields(self) -> None:
        st = state()
        self.assertTrue(st.have_packet)
        self.assertEqual(st.mode, "RUN")
        self.assertEqual(st.sample_temps[2], None, "S2 is invalid on the wire -> None")
        self.assertEqual(st.sample_temps[0], 1.0)
        self.assertEqual(st.sample_resistance[4], 121.0)
        self.assertIsNone(st.sample_resistance[1])
        self.assertTrue(st.heater_temp_valid(0))
        self.assertFalse(st.heater_temp_valid(2))
        self.assertIs(st.fallback, False)
        m0, m1 = st.motors
        self.assertTrue(m0.enabled and m0.zeroed and m0.healthy)
        self.assertFalse(m1.enabled)
        self.assertIs(m1.zeroed, False)
        self.assertTrue(m1.moving)
        self.assertEqual(st.layout.motor_samples[1], (4, 5, 6, 7))

    def test_empty_state_defaults(self) -> None:
        st = OnboardState()
        self.assertFalse(st.have_packet)
        self.assertEqual(len(st.motors), 2)
        self.assertFalse(st.motor(1).present)


class GatingTests(unittest.TestCase):
    def test_silence_blocks_everything(self) -> None:
        st = state(silence=True)
        for reason in (gating.arm_reason(st), gating.disarm_reason(st), gating.generic_reason(st),
                       gating.heater_reason(st, 0), gating.motion_reason(st, 0, needs_zero=True),
                       gating.enable_reason(st, 0)):
            self.assertEqual(reason, gating.SILENCE)

    def test_mode_gates(self) -> None:
        run = state(mode="RUN")
        standby = state(mode="STANDBY")
        safe = state(mode="SAFE")
        self.assertIsNotNone(gating.arm_reason(run))
        self.assertIsNone(gating.arm_reason(standby))
        self.assertIsNone(gating.disarm_reason(run))
        self.assertIsNotNone(gating.disarm_reason(standby))
        self.assertIsNone(gating.exit_safe_reason(safe))
        self.assertIsNotNone(gating.exit_safe_reason(run))
        self.assertIn("ARM", gating.motion_reason(standby, 0, needs_zero=False) or "")
        self.assertIn("SAFE", gating.motion_reason(safe, 0, needs_zero=False) or "")
        self.assertIn("ARM", gating.heater_reason(standby, 0) or "")

    # MUTATION: make _mode_reason always return None and confirm
    # test_mode_gates fails on the STANDBY motion assertion.

    def test_motion_gates_enable_zero_and_fallback(self) -> None:
        st = state()
        self.assertIsNone(gating.motion_reason(st, 0, needs_zero=True))
        self.assertIn("not enabled", gating.motion_reason(st, 1, needs_zero=True) or "")
        enabled_unzeroed = state(m1="en:1|zeroed:0")
        self.assertIn("not zeroed", gating.motion_reason(enabled_unzeroed, 1, needs_zero=True) or "")
        self.assertIsNone(gating.motion_reason(enabled_unzeroed, 1, needs_zero=False),
                          "relative jog is allowed before zeroing")
        fallback = state(ctrl="fallback:1|link_loss_s:30.0")
        self.assertIn("fallback", gating.motion_reason(fallback, 0, needs_zero=True) or "")

    # MUTATION: drop the `needs_zero and motor.zeroed is False` clause in
    # motion_reason and confirm test_motion_gates_enable_zero_and_fallback
    # fails on the "not zeroed" assertion.

    def test_unknown_facts_never_block(self) -> None:
        old_firmware = state(m0="en:1", m1="en:1")  # no zeroed key
        self.assertIsNone(gating.motion_reason(old_firmware, 0, needs_zero=True))
        self.assertIsNone(gating.motion_reason(OnboardState(), 0, needs_zero=True))
        self.assertIsNone(gating.heater_reason(OnboardState(), 3))
        self.assertIsNone(gating.arm_reason(OnboardState()))

    def test_heater_gates_on_sample_validity(self) -> None:
        st = state()
        self.assertIsNone(gating.heater_reason(st, 0))
        self.assertIn("S2", gating.heater_reason(st, 2) or "")
        self.assertIn("S2", gating.all_heaters_reason(st) or "")
        self.assertIsNone(gating.heater_reason(st, 2, needs_temperature=False))

    def test_debug_arm_unlocks_open_loop_duty_only(self) -> None:
        # ARM_DEBUG (CTRL debug:1): the onboard accepts open-loop duty
        # without PT100 feedback, so duty_reason must stop predicting a
        # NACK — but closed-loop targets still require valid feedback.
        armed = state(ctrl="fallback:0|debug:1")
        self.assertIs(armed.debug_armed, True)
        self.assertIsNone(gating.duty_reason(armed, 2))
        self.assertIsNone(gating.all_duty_reason(armed))
        self.assertIn("S2", gating.heater_reason(armed, 2) or "",
                      "SET_TEMP_TARGET still needs feedback under debug arm")
        self.assertIn("S2", gating.all_heaters_reason(armed) or "")
        # Debug arm lifts only the feedback gate — mode and silence stay.
        standby_armed = state(mode="STANDBY", ctrl="fallback:0|debug:1")
        self.assertIn("ARM", gating.duty_reason(standby_armed, 2) or "")
        silent_armed = state(ctrl="fallback:0|debug:1", silence=True)
        self.assertEqual(gating.duty_reason(silent_armed, 2), gating.SILENCE)
        # Not armed (or old firmware without the key): duty stays gated on
        # feedback, with a hint pointing at ARM_DEBUG.
        unarmed = state(ctrl="fallback:0|debug:0")
        self.assertIs(unarmed.debug_armed, False)
        self.assertIn("ARM_DEBUG", gating.duty_reason(unarmed, 2) or "")
        old_firmware = state()  # default ctrl has no debug key
        self.assertIsNone(old_firmware.debug_armed)
        self.assertIn("S2", gating.duty_reason(old_firmware, 2) or "")

    def test_thermal_shutdown_names_itself_in_motion_gating(self) -> None:
        # A hot driver is disabled by the onboard safety; the reason must say
        # so instead of the generic "not enabled". ENABLE itself stays
        # available (it is the re-arm path).
        hot = state(m0="en:0|zeroed:1|therm:hot")
        reason = gating.motion_reason(hot, 0, needs_zero=False) or ""
        self.assertIn("over-temperature", reason)
        self.assertIn("ENABLE", reason)
        self.assertIsNone(gating.enable_reason(hot, 0))
        # therm parses through to MotorState.
        self.assertEqual(hot.motor(0).thermal, "hot")

    def test_enable_gate_on_failed_motor(self) -> None:
        st = state()
        self.assertIsNone(gating.enable_reason(st, 0))
        self.assertIn("FAILED", gating.enable_reason(st, 1) or "")


if __name__ == "__main__":
    unittest.main()
