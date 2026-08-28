"""Alarm model (redesign spec §5.2). No Qt."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.alarms import AlarmModel, evaluate  # noqa: E402
from app.gui.state import OnboardState, state_from_packet  # noqa: E402
from app.protocol import parse_telemetry_csv  # noqa: E402


def frame(status="SD_OK|USB_OK|LINK_OK|OVERTEMP_OK|SAMPLE_TEMP_OK|HEATER_ACTIVE|SEQ_READY",
          ctrl="fallback:0|queue:0", comps="DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK",
          m1="mv:0") -> str:
    return (f"DATA,coatheal-1-1,1,2026-08-28T00:00:00Z,1,20,1000,0.1,1,2,3,4,nan,6,7,8,"
            f"HEATER_DUTY=0|0|0|0|0|0,PHASE=ASCENT,MODE=RUN,STATUS={status},"
            f"SENSOR_VALID=AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:0|S5:1|S6:1|S7:1,"
            f"COMPONENT_STATE={comps},CTRL={ctrl},"
            f"STEPPER0=pos:0|tgt:0|hz:100|us:4|en:1|mv:0|hold:0|hold_s:0|pulses:0|src:init,"
            f"STEPPER1=pos:0|tgt:0|hz:100|us:4|en:1|{m1}|hold:0|hold_s:0|pulses:0|src:init")


def state(link_age=0.5, silence=False, **kw) -> OnboardState:
    return state_from_packet(parse_telemetry_csv(frame(**kw)), silence=silence, link_age_s=link_age)


class EvaluateTests(unittest.TestCase):
    def test_quiet_when_all_good(self) -> None:
        self.assertEqual(evaluate(state()), [])

    def test_each_source_raises_its_alarm(self) -> None:
        keys = {a.key for a in evaluate(state(
            status="SD_FAIL|USB_OK|LINK_OK|OVERTEMP_FAIL|SAMPLE_TEMP_FAIL|HEATER_INHIBITED|SEQ_PAUSED|ENERGY_FAIL",
            ctrl="fallback:1|link_loss_s:42.0|queue:250",
            comps="DPS310:STALE|ADS1115:OK|SEQUENT_RTD:DEGRADED|MOTOR0:FAILED|MOTOR1:OK|PWM:OK",
            m1="mv:1", link_age=7.0))}
        self.assertEqual(keys, {"OVERTEMP", "SAMPLE_TEMP", "FALLBACK", "SEQ_PAUSED", "ENERGY",
                                "MOTOR0", "HEATERS_INHIBITED", "SENSOR", "RX_QUEUE", "LINK"})

    # MUTATION: remove the OVERTEMP branch from evaluate and confirm
    # test_each_source_raises_its_alarm fails on the missing key.

    def test_texts_carry_the_detail(self) -> None:
        alarms = {a.key: a for a in evaluate(state(
            status="SD_OK|USB_OK|LINK_OK|SAMPLE_TEMP_FAIL|HEATER_INHIBITED",
            comps="DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK", m1="mv:1"))}
        self.assertIn("S4", alarms["SAMPLE_TEMP"].text)
        self.assertIn("M1", alarms["HEATERS_INHIBITED"].text)
        self.assertEqual(alarms["HEATERS_INHIBITED"].severity, "amber")

    def test_link_alarm_suppressed_during_silence(self) -> None:
        self.assertIn("LINK", {a.key for a in evaluate(state(link_age=30.0))})
        self.assertNotIn("LINK", {a.key for a in evaluate(state(link_age=30.0, silence=True))})

    # MUTATION: drop `not state.silence and` from the LINK condition and
    # confirm test_link_alarm_suppressed_during_silence fails.

    def test_link_alarm_without_any_packet(self) -> None:
        self.assertEqual([a.key for a in evaluate(OnboardState(link_age_s=9.0))], ["LINK"])
        self.assertEqual(evaluate(OnboardState()), [])


class ModelTests(unittest.TestCase):
    def test_ack_persists_until_condition_clears_then_rearms(self) -> None:
        model = AlarmModel()
        bad = state(status="SD_OK|USB_OK|LINK_OK|OVERTEMP_FAIL")
        good = state()
        self.assertEqual([a.key for a in model.update(bad)], ["OVERTEMP"])
        self.assertEqual(model.new_keys, ["OVERTEMP"])
        self.assertEqual(model.unacked_count, 1)
        model.acknowledge("OVERTEMP")
        active = model.update(bad)
        self.assertTrue(active[0].acked, "acked alarm stays listed, dimmed")
        self.assertEqual(model.new_keys, [])
        self.assertEqual(model.unacked_count, 0)
        self.assertEqual(model.update(good), [])
        raised = model.update(bad)
        self.assertFalse(raised[0].acked, "returning condition is a new alarm")
        self.assertEqual(model.new_keys, ["OVERTEMP"])

    # MUTATION: remove the loop that deletes cleared keys from self._acked
    # in AlarmModel.update and confirm the "returning condition" assertion fails.

    def test_ack_all(self) -> None:
        model = AlarmModel()
        model.update(state(status="SD_FAIL|OVERTEMP_FAIL"))
        self.assertEqual(model.unacked_count, 2)
        model.acknowledge_all()
        self.assertEqual(model.unacked_count, 0)


if __name__ == "__main__":
    unittest.main()
