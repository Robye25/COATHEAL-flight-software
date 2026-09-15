"""Motor-group layout (GET_LAYOUT), the >40 °C heating confirmation and the
SI motion units (owner list 2026-09-15). No Qt."""
from __future__ import annotations

import dataclasses
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui import gating  # noqa: E402
from app.gui.alarms import evaluate  # noqa: E402
from app.gui.state import DEFAULT_LAYOUT, Layout, parse_layout, parse_lead_mm, state_from_packet  # noqa: E402
from app.protocol import (  # noqa: E402
    FULL_STEPS_PER_MM, LEAD_MM_PER_REV, MAX_ACCEL_MM_S2, MAX_SPEED_HZ, MAX_SPEED_MM_S, OVERTEMP_LATCH_C,
    TARGET_MAX_C, hz_from_mm_s, mm_s_from_hz, parse_telemetry_csv, validate_accel_mm_s2,
    validate_speed_mm_s, validate_temperature_target,
)

# The reply the onboard unit test pins for a bench wiring (tests/unit/test_suite.cpp).
FIRMWARE_REPLY = ("samples=8;heaters=6;motor0=0,1,2,3;motor1=4,5,6,7;heater_samples=0,1,2,4,5,6;"
                  "clicks=0,4;rtd_channels=8,2,3,4,5,7,1,6;heater_lines=19,13,6,5,24,23")
# Three specimens on motor 0 (S2 unheated), five on motor 1 (S7 unheated).
UNEVEN_REPLY = ("samples=8;heaters=6;motor0=0,1,2;motor1=3,4,5,6,7;heater_samples=0,1,3,4,5,6;"
                "clicks=0,3;rtd_channels=1,2,3,4,5,6,7,8;heater_lines=19,13,6,5,24,23")


def frame(samples: str = "20,21,22,23,24,25,26,27",
          valid: str = "S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1",
          status: str = "SD_OK|SAMPLE_TEMP_OK") -> str:
    return (f"DATA,coatheal-1787760547-1,1,2026-08-28T00:00:00Z,1,20,1000,0.1,{samples},"
            f"HEATER_DUTY=0|0|0|0|0|0,PHASE=ASCENT,MODE=RUN,STATUS={status},SENSOR_VALID={valid}")


class ParseLayoutTests(unittest.TestCase):
    def test_firmware_reply(self) -> None:
        layout = parse_layout(FIRMWARE_REPLY)
        self.assertIsNotNone(layout)
        self.assertTrue(layout.reported)
        self.assertEqual(layout.motor_samples, ((0, 1, 2, 3), (4, 5, 6, 7)))
        self.assertEqual(layout.heater_samples, (0, 1, 2, 4, 5, 6))
        self.assertEqual(layout.clicks, (0, 4))
        self.assertEqual(layout.rtd_channels, (8, 2, 3, 4, 5, 7, 1, 6))
        self.assertEqual(layout.heater_lines, (19, 13, 6, 5, 24, 23))
        # S3 has no heater; H3 reads S4, on motor 1.
        self.assertIsNone(layout.heater_of_sample(3))
        self.assertEqual(layout.sample_of_heater(3), 4)
        self.assertEqual(layout.motor_of_heater(3), 1)
        self.assertEqual(layout.heaters_of_motor(0), (0, 1, 2))
        self.assertEqual(layout.heaters_of_motor(1), (3, 4, 5))
        self.assertNotEqual(layout, DEFAULT_LAYOUT)

    def test_uneven_groups(self) -> None:
        layout = parse_layout(UNEVEN_REPLY)
        self.assertEqual(layout.motor_samples, ((0, 1, 2), (3, 4, 5, 6, 7)))
        self.assertEqual(layout.heaters_of_motor(0), (0, 1))
        self.assertEqual(layout.heaters_of_motor(1), (2, 3, 4, 5))
        self.assertEqual(layout.motor_of_sample(3), 1)
        self.assertEqual(layout.click_of_sample(0), 1)
        self.assertEqual(layout.click_of_sample(3), 2)
        self.assertIsNone(layout.click_of_sample(4))
        self.assertEqual(layout.as_dict()["motor_samples"], [[0, 1, 2], [3, 4, 5, 6, 7]])

    def test_the_schematic_reply_equals_the_default_but_is_marked_reported(self) -> None:
        body = ("samples=8;heaters=6;motor0=0,1,2,3;motor1=4,5,6,7;heater_samples=0,1,2,3,4,5;"
                "clicks=0,4;rtd_channels=1,2,3,4,5,6,7,8;heater_lines=19,13,6,5,24,23")
        layout = parse_layout(body)
        self.assertEqual(dataclasses.replace(layout, reported=False), DEFAULT_LAYOUT)

    def test_unusable_replies(self) -> None:
        good = dict(piece.split("=", 1) for piece in UNEVEN_REPLY.split(";"))

        def body(**changes: str) -> str:
            values = {**good, **changes}
            return ";".join(f"{k}={v}" for k, v in values.items() if v is not None)

        self.assertIsNotNone(parse_layout(body()))
        cases = {
            "a sample in no group": body(motor1="3,4,5,6"),
            "a sample in both groups": body(motor1="2,3,4,5,6,7"),
            "a sample past the count": body(motor1="3,4,5,6,7,8"),
            "five heaters": body(heater_samples="0,1,3,4,5"),
            "two heaters on one sample": body(heater_samples="0,1,3,4,5,5"),
            "a heater on a missing sample": body(heater_samples="0,1,3,4,5,9"),
            "not a number": body(motor0="0,one,2"),
            "a bad sample count": body(samples="eight"),
        }
        for name, text in cases.items():
            self.assertIsNone(parse_layout(text), name)
        self.assertIsNone(parse_layout(""))
        self.assertIsNone(parse_layout("unknown command"))

    # MUTATION: drop the `covered != list(range(samples))` check from
    # parse_layout and confirm test_unusable_replies fails on "a sample in no group".


class LayoutStateTests(unittest.TestCase):
    """Heater validity, gating and alarms follow the reported layout, not
    heater i = sample i."""

    def setUp(self) -> None:
        self.layout = parse_layout(FIRMWARE_REPLY)   # H3 reads S4, S3 unheated

    def test_heater_validity_follows_its_sample(self) -> None:
        pkt = parse_telemetry_csv(frame(samples="20,21,22,nan,24,25,26,27",
                                        valid="S0:1|S1:1|S2:1|S3:0|S4:1|S5:1|S6:1|S7:1"))
        st = state_from_packet(pkt, layout=self.layout)
        self.assertTrue(st.heater_temp_valid(3), "H3 reads S4; the unheated S3 is irrelevant")
        self.assertIsNone(gating.heater_reason(st, 3))
        self.assertIsNone(gating.all_heaters_reason(st))
        # With the schematic layout the same frame blocks H3.
        schematic = state_from_packet(pkt)
        self.assertFalse(schematic.heater_temp_valid(3))
        self.assertIn("S3", gating.heater_reason(schematic, 3) or "")

    def test_reasons_and_alarm_name_the_layout_sample(self) -> None:
        pkt = parse_telemetry_csv(frame(samples="20,21,22,23,nan,25,26,27",
                                        valid="S0:1|S1:1|S2:1|S3:1|S4:0|S5:1|S6:1|S7:1",
                                        status="SD_OK|SAMPLE_TEMP_FAIL"))
        st = state_from_packet(pkt, layout=self.layout)
        self.assertFalse(st.heater_temp_valid(3))
        self.assertIn("S4", gating.heater_reason(st, 3) or "")
        self.assertIn("H3", gating.heater_reason(st, 3) or "")
        self.assertEqual(gating.all_heaters_reason(st), "no valid temperature on S4")
        alarm = next(a for a in evaluate(st) if a.key == "SAMPLE_TEMP")
        self.assertIn("S4", alarm.text)
        self.assertNotIn("S3", alarm.text)

    def test_out_of_range_heater_is_invalid(self) -> None:
        st = state_from_packet(parse_telemetry_csv(frame()), layout=self.layout)
        self.assertFalse(st.heater_temp_valid(6))
        self.assertFalse(st.heater_temp_valid(-1))
        self.assertIsNone(Layout().sample_of_heater(6))


class HeatingConfirmationTests(unittest.TestCase):
    def test_threshold_is_strictly_above_40(self) -> None:
        self.assertEqual(gating.CONFIRM_ABOVE_C, 40.0)
        self.assertIsNone(gating.heating_question("SET_TEMP_TARGET 0 40"))
        self.assertIsNone(gating.heating_question("SET_TEMP_TARGET 0 40.000"))
        question = gating.heating_question("SET_TEMP_TARGET 0 40.1")
        self.assertIn("H0", question)
        self.assertIn("40.1 °C", question)
        self.assertIn("above 40 °C", question)

    def test_every_heating_command(self) -> None:
        self.assertIn("every heater", gating.heating_question("SET_ALL_TEMP_TARGETS 55") or "")
        self.assertIn("H2", gating.heating_question("PID_TUNE_START 2 45 0.5 4") or "")
        self.assertIsNotNone(gating.heating_question("set_temp_target 5 75"), "verbs are case-insensitive")
        self.assertIsNone(gating.heating_question("PID_TUNE_START 2 40 0.5 4"))
        self.assertIsNone(gating.heating_question("SET_ALL_TEMP_TARGETS 25"))

    def test_other_commands_never_ask(self) -> None:
        for command in ("", "   ", "CLEAR_TEMP_TARGET 0", "SET_HEATER_DUTY 0 0.9", "SET_PID 0 90 1 1",
                        "STEPPER_MOVETO_MM 0 60", "SET_TEMP_TARGET 0", "SET_TEMP_TARGET 0 hot",
                        "SET_ALL_TEMP_TARGETS"):
            self.assertIsNone(gating.heating_question(command), command)

    # MUTATION: change `if not target > CONFIRM_ABOVE_C` to `>=` semantics
    # (`if target < CONFIRM_ABOVE_C`) and confirm test_threshold_is_strictly_above_40 fails.


class TemperatureLimitTests(unittest.TestCase):
    def test_latch_and_target_ceiling(self) -> None:
        self.assertEqual(OVERTEMP_LATCH_C, 80.0)
        self.assertEqual(TARGET_MAX_C, 75.0)
        self.assertLess(TARGET_MAX_C, OVERTEMP_LATCH_C)
        self.assertEqual(validate_temperature_target(75.0), (True, "75.000"))
        self.assertFalse(validate_temperature_target(75.01)[0])


class SiMotionUnitTests(unittest.TestCase):
    def test_conversions_follow_the_lead(self) -> None:
        # 1 mm lead (owner 2026-09-15), 200 full steps per revolution: 200
        # full steps per mm.
        self.assertEqual(LEAD_MM_PER_REV, 1.0)
        self.assertEqual(FULL_STEPS_PER_MM, 200)
        self.assertEqual(MAX_SPEED_HZ, 100.0)
        self.assertEqual(hz_from_mm_s(MAX_SPEED_MM_S), MAX_SPEED_HZ)
        self.assertEqual(hz_from_mm_s(0.25), 50.0)
        self.assertEqual(mm_s_from_hz(100.0), 0.5)
        for mm_s in (0.001, 0.123, 0.5):
            self.assertAlmostEqual(mm_s_from_hz(hz_from_mm_s(mm_s)), mm_s)

    def test_speed_validator(self) -> None:
        self.assertEqual(validate_speed_mm_s(0.25), (True, "50.000"))
        self.assertEqual(validate_speed_mm_s(0.5), (True, "100.000"))
        for bad in (0.0, -0.1, 0.51, math.nan, math.inf, "fast", None):
            ok, message = validate_speed_mm_s(bad)
            self.assertFalse(ok, bad)
            self.assertTrue("mm/s" in message or "numeric" in message, message)

    def test_accel_validator(self) -> None:
        self.assertEqual(MAX_ACCEL_MM_S2, 25.0)
        self.assertEqual(validate_accel_mm_s2(4.0), (True, "800.0"))
        self.assertEqual(validate_accel_mm_s2(25.0), (True, "5000.0"))
        for bad in (0.0, -1.0, 25.1, math.nan, "quick"):
            self.assertFalse(validate_accel_mm_s2(bad)[0], bad)

    # MUTATION: set protocol.LEAD_MM_PER_REV back to 2.0 and confirm
    # test_conversions_follow_the_lead and test_speed_validator fail.


class LeadReportTests(unittest.TestCase):
    """GET_LAYOUT's `lead_mm=` (added onboard 2026-09-15)."""

    def test_lead_is_read_when_present(self) -> None:
        self.assertEqual(parse_lead_mm(FIRMWARE_REPLY + ";lead_mm=1"), 1.0)
        self.assertEqual(parse_lead_mm(FIRMWARE_REPLY + ";lead_mm=2.5"), 2.5)
        self.assertIsNotNone(parse_layout(FIRMWARE_REPLY + ";lead_mm=1"), "the extra key keeps the layout usable")

    def test_absent_or_unusable_lead_is_none(self) -> None:
        self.assertIsNone(parse_lead_mm(FIRMWARE_REPLY))
        for bad in ("lead_mm=", "lead_mm=fast", "lead_mm=0", "lead_mm=-1", "lead_mm=nan", "lead_mm=inf"):
            self.assertIsNone(parse_lead_mm(f"{FIRMWARE_REPLY};{bad}"), bad)


if __name__ == "__main__":
    unittest.main()
