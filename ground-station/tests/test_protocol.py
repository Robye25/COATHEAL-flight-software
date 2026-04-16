import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import (
    PullEvent, StepperSnapshot, TelemetryParseError, build_ack,
    parse_command_response, parse_pull_event, parse_telemetry_csv,
    validate_duty, validate_heater_index, validate_microstep,
    validate_revolutions, validate_speed_hz, validate_stepper_move,
    validate_tick_hz,
)


# Legacy Rev-A data frame: 9 sample temps, no stepper segment, short
# STATUS flag list. Every line here must still parse after the Rev-B
# rewrite — old SD-card CSV logs go through this path.
LEGACY_DATA_9_SAMPLES = (
    "DATA,session-1,1,2026-03-31T12:00:00Z,1,-30.00,150.00,20.00,1.23,5.00,"
    "-30.00,-30.10,-29.90,-29.80,-29.70,-29.60,-29.50,-29.40,-29.30,"
    "HEATER_DUTY=0.5|0.5|0.0|0.0|0.0|0.0|0.0|0.0|0.0|0.0,"
    "PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK"
)

# Three-sample legacy frame (unit-test style, short arity). Kept to exercise
# the "sample count inferred from HEATER_DUTY position" path.
LEGACY_DATA = (
    "DATA,session-1,1,2026-03-31T12:00:00Z,1,-30.00,150.00,20.00,1.23,5.00,"
    "-30.00,-30.10,-29.90,HEATER_DUTY=0.5|0.5|0.0,"
    "PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK"
)

# Legacy single-STEPPER frame. Must still parse, and `packet.steppers`
# should be a 1-element list mirroring `packet.stepper`.
STEPPER_DATA = (
    "DATA,sess-2,7,2026-04-13T01:02:03Z,1,-25.00,180.00,22.00,0.80,4.00,"
    "-30.00,-29.80,-29.90,HEATER_DUTY=0.10|0.20|0.30,"
    "PHASE=ASCENT_HOLD_-30C,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK,"
    "STEPPER=pos:1234|tgt:2000|hz:800|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:1234|src:cmd:MOVE"
)

# Rev-B production frame: 8 sample temps + 9 heater duties + dual-motor
# STEPPER0/STEPPER1 segments + new phase token + new STATUS bits.
DUAL_STEPPER_DATA = (
    "DATA,sess-b,42,2026-04-16T10:20:30Z,1,-10.00,140.00,18.00,0.01,3.00,"
    "-5.00,-5.10,-5.20,-5.30,-5.40,-5.50,-5.60,-5.70,"
    "HEATER_DUTY=0.00|0.10|0.20|0.30|0.40|0.50|0.60|0.70|0.05,"
    "PHASE=FLOAT,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK"
    "|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_INHIBITED,"
    "STEPPER0=pos:100|tgt:200|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:100|src:cmd:MOVE,"
    "STEPPER1=pos:-50|tgt:-50|hz:200|us:8|en:1|mv:0|hold:1|hold_s:3.5|pulses:50|src:phase:FLOAT"
)


class DataFrameTests(unittest.TestCase):
    def test_parse_valid_packet(self) -> None:
        pkt = parse_telemetry_csv(LEGACY_DATA)
        self.assertEqual(pkt.session_id, "session-1")
        self.assertEqual(pkt.seq, 1)
        self.assertEqual(pkt.phase, "ASCENT_HOLD_-30C")
        self.assertEqual(len(pkt.heater_duty), 3)
        self.assertIsNone(pkt.stepper)
        self.assertEqual(pkt.steppers, [])
        self.assertEqual(pkt.mode, "")

    def test_parse_legacy_9_samples_frame(self) -> None:
        # Rev-A 9-sample frame must still round-trip; old SD logs need it.
        pkt = parse_telemetry_csv(LEGACY_DATA_9_SAMPLES)
        self.assertEqual(len(pkt.sample_temps_c), 9)
        self.assertEqual(len(pkt.heater_duty), 10)
        self.assertEqual(pkt.phase, "ASCENT_HOLD_-30C")
        self.assertEqual(pkt.steppers, [])

    def test_parse_invalid_packet(self) -> None:
        with self.assertRaises(TelemetryParseError):
            parse_telemetry_csv("broken")

    def test_build_ack(self) -> None:
        self.assertEqual(build_ack("session-1", 42), "ACK,session-1,42\n")

    def test_parse_with_stepper(self) -> None:
        pkt = parse_telemetry_csv(STEPPER_DATA)
        self.assertEqual(pkt.mode, "RUN")
        self.assertIsNotNone(pkt.stepper)
        s = pkt.stepper
        assert isinstance(s, StepperSnapshot)
        self.assertEqual(s.position, 1234)
        self.assertEqual(s.target, 2000)
        self.assertEqual(s.microstep, 16)
        self.assertTrue(s.enabled)
        self.assertTrue(s.moving)
        self.assertFalse(s.holding)
        self.assertEqual(s.source, "cmd:MOVE")
        # Legacy STEPPER= also surfaces as a 1-element `steppers` list.
        self.assertEqual(len(pkt.steppers), 1)
        self.assertEqual(pkt.steppers[0]["motor_id"], 0)
        self.assertEqual(pkt.steppers[0]["position"], 1234)

    def test_stepper_unknown_key_ignored(self) -> None:
        # Forward-compat: unknown keys silently dropped.
        line = STEPPER_DATA.replace("|src:cmd:MOVE", "|src:cmd:MOVE|newfield:42")
        pkt = parse_telemetry_csv(line)
        self.assertIsNotNone(pkt.stepper)

    def test_stepper_malformed_pair_rejected(self) -> None:
        line = STEPPER_DATA.replace("pos:1234", "posXX1234")
        with self.assertRaises(TelemetryParseError):
            parse_telemetry_csv(line)

    # ── Rev-B: dual stepper + 8-sample frame ─────────────────────────────
    def test_parse_dual_stepper_frame(self) -> None:
        pkt = parse_telemetry_csv(DUAL_STEPPER_DATA)
        self.assertEqual(pkt.session_id, "sess-b")
        self.assertEqual(pkt.seq, 42)
        self.assertEqual(pkt.phase, "FLOAT")
        self.assertEqual(len(pkt.sample_temps_c), 8)
        self.assertEqual(len(pkt.heater_duty), 9)
        # Two motors, in index order.
        self.assertEqual(len(pkt.steppers), 2)
        self.assertEqual(pkt.steppers[0]["motor_id"], 0)
        self.assertEqual(pkt.steppers[0]["position"], 100)
        self.assertEqual(pkt.steppers[0]["target"], 200)
        self.assertTrue(pkt.steppers[0]["moving"])
        self.assertEqual(pkt.steppers[1]["motor_id"], 1)
        self.assertEqual(pkt.steppers[1]["position"], -50)
        self.assertTrue(pkt.steppers[1]["holding"])
        # Legacy single `.stepper` mirrors motor 0 for back-compat.
        self.assertIsNotNone(pkt.stepper)
        assert isinstance(pkt.stepper, StepperSnapshot)
        self.assertEqual(pkt.stepper.position, 100)
        # New STATUS bits are in the status string verbatim.
        self.assertIn("RS485_OK", pkt.status)
        self.assertIn("HEATER_INHIBITED", pkt.status)

    def test_dual_stepper_handles_out_of_order_indices(self) -> None:
        # If M1's segment comes before M0's on the wire, the parser sorts
        # by motor_id so consumers see a stable ordering.
        swapped = DUAL_STEPPER_DATA.replace(
            ",STEPPER0=pos:100", ",STEPPER_TMP=pos:100"
        ).replace(
            ",STEPPER1=pos:-50", ",STEPPER0=pos:-50"
        ).replace(
            ",STEPPER_TMP=pos:100", ",STEPPER1=pos:100"
        )
        pkt = parse_telemetry_csv(swapped)
        self.assertEqual(len(pkt.steppers), 2)
        self.assertEqual(pkt.steppers[0]["motor_id"], 0)
        self.assertEqual(pkt.steppers[0]["position"], -50)
        self.assertEqual(pkt.steppers[1]["motor_id"], 1)
        self.assertEqual(pkt.steppers[1]["position"], 100)


class PullEventTests(unittest.TestCase):
    def test_parse_valid(self) -> None:
        line = "EVT,PULL,sess-b,3,1,2026-04-16T10:21:00Z,2400,12.00,0|1|2|3"
        ev = parse_pull_event(line)
        self.assertIsInstance(ev, PullEvent)
        self.assertEqual(ev.session_id, "sess-b")
        self.assertEqual(ev.pull_id, 3)
        self.assertEqual(ev.motor_id, 1)
        self.assertEqual(ev.steps_moved, 2400)
        self.assertAlmostEqual(ev.hold_s, 12.0)
        self.assertEqual(ev.samples, [0, 1, 2, 3])

    def test_parse_empty_samples(self) -> None:
        # Onboard encodes "no specimens touched" as a literal "-".
        line = "EVT,PULL,sess-b,4,0,2026-04-16T10:22:00Z,-1200,0.00,-"
        ev = parse_pull_event(line)
        self.assertEqual(ev.samples, [])
        self.assertEqual(ev.steps_moved, -1200)
        self.assertEqual(ev.motor_id, 0)

    def test_rejects_non_pull_frame(self) -> None:
        with self.assertRaises(TelemetryParseError):
            parse_pull_event("EVT,CYCLE,sess,1,ts,70,3600,0.08,2")

    def test_rejects_malformed_samples(self) -> None:
        with self.assertRaises(TelemetryParseError):
            parse_pull_event("EVT,PULL,s,1,0,ts,0,0.0,0|x|2")

    def test_rejects_short_frame(self) -> None:
        with self.assertRaises(TelemetryParseError):
            parse_pull_event("EVT,PULL,s,1,0,ts,0")


class CommandResponseTests(unittest.TestCase):
    def test_ack(self) -> None:
        r = parse_command_response("ACK,PING,pong")
        self.assertTrue(r.ok); self.assertEqual(r.command, "PING"); self.assertEqual(r.body, "pong")

    def test_nack(self) -> None:
        r = parse_command_response("NACK,SET_TICK_HZ,out of range")
        self.assertFalse(r.ok); self.assertEqual(r.command, "SET_TICK_HZ"); self.assertIn("out of range", r.error)

    def test_malformed(self) -> None:
        r = parse_command_response("weird reply")
        self.assertFalse(r.ok); self.assertIn("unrecognised", r.error)

    def test_empty(self) -> None:
        r = parse_command_response("")
        self.assertFalse(r.ok)


class ValidatorTests(unittest.TestCase):
    def test_duty(self) -> None:
        self.assertTrue(validate_duty(0.0)[0])
        self.assertTrue(validate_duty(1.0)[0])
        self.assertFalse(validate_duty(1.01)[0])
        self.assertFalse(validate_duty(-0.01)[0])
        self.assertFalse(validate_duty("xx")[0])

    def test_heater_index(self) -> None:
        # Rev-B: 9 heater channels (0..7 samples + 8 BOX). Default count=9.
        self.assertTrue(validate_heater_index(0)[0])
        self.assertTrue(validate_heater_index(8)[0])
        self.assertFalse(validate_heater_index(9)[0])
        self.assertFalse(validate_heater_index(-1)[0])
        # Explicit count still honoured for callers that pre-set it.
        self.assertTrue(validate_heater_index(9, count=10)[0])

    def test_tick_hz(self) -> None:
        self.assertTrue(validate_tick_hz(0.1)[0])
        self.assertTrue(validate_tick_hz(5.0)[0])
        self.assertFalse(validate_tick_hz(0.05)[0])
        self.assertFalse(validate_tick_hz(5.01)[0])

    def test_speed_hz(self) -> None:
        self.assertTrue(validate_speed_hz(1)[0])
        self.assertTrue(validate_speed_hz(5000)[0])
        self.assertFalse(validate_speed_hz(0)[0])
        self.assertFalse(validate_speed_hz(5001)[0])

    def test_microstep(self) -> None:
        for n in (1, 2, 4, 8, 16, 32):
            self.assertTrue(validate_microstep(n)[0], msg=f"{n}")
        for n in (0, 3, 64):
            self.assertFalse(validate_microstep(n)[0], msg=f"{n}")

    def test_stepper_move(self) -> None:
        self.assertTrue(validate_stepper_move(100)[0])
        self.assertTrue(validate_stepper_move(-200000)[0])
        self.assertFalse(validate_stepper_move(300000)[0])
        self.assertFalse(validate_stepper_move("abc")[0])

    def test_revolutions(self) -> None:
        self.assertTrue(validate_revolutions(0.0)[0])
        self.assertTrue(validate_revolutions(-12.5)[0])
        self.assertFalse(validate_revolutions(2e6)[0])


if __name__ == "__main__":
    unittest.main()
