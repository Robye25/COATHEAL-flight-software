import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import (
    StepperSnapshot, TelemetryParseError, build_ack, parse_command_response,
    parse_telemetry_csv, validate_duty, validate_heater_index, validate_microstep,
    validate_revolutions, validate_speed_hz, validate_stepper_move, validate_tick_hz,
)


LEGACY_DATA = (
    "DATA,session-1,1,2026-03-31T12:00:00Z,1,-30.00,150.00,20.00,1.23,5.00,"
    "-30.00,-30.10,-29.90,HEATER_DUTY=0.5|0.5|0.0,"
    "PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK"
)

STEPPER_DATA = (
    "DATA,sess-2,7,2026-04-13T01:02:03Z,1,-25.00,180.00,22.00,0.80,4.00,"
    "-30.00,-29.80,-29.90,HEATER_DUTY=0.10|0.20|0.30,"
    "PHASE=ASCENT_HOLD_-30C,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK,"
    "STEPPER=pos:1234|tgt:2000|hz:800|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:1234|src:cmd:MOVE"
)


class DataFrameTests(unittest.TestCase):
    def test_parse_valid_packet(self) -> None:
        pkt = parse_telemetry_csv(LEGACY_DATA)
        self.assertEqual(pkt.session_id, "session-1")
        self.assertEqual(pkt.seq, 1)
        self.assertEqual(pkt.phase, "ASCENT_HOLD_-30C")
        self.assertEqual(len(pkt.heater_duty), 3)
        self.assertIsNone(pkt.stepper)
        self.assertEqual(pkt.mode, "")

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

    def test_stepper_unknown_key_ignored(self) -> None:
        # Forward-compat: unknown keys silently dropped.
        line = STEPPER_DATA.replace("|src:cmd:MOVE", "|src:cmd:MOVE|newfield:42")
        pkt = parse_telemetry_csv(line)
        self.assertIsNotNone(pkt.stepper)

    def test_stepper_malformed_pair_rejected(self) -> None:
        line = STEPPER_DATA.replace("pos:1234", "posXX1234")
        with self.assertRaises(TelemetryParseError):
            parse_telemetry_csv(line)


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
        self.assertTrue(validate_heater_index(0)[0])
        self.assertTrue(validate_heater_index(9)[0])
        self.assertFalse(validate_heater_index(10)[0])
        self.assertFalse(validate_heater_index(-1)[0])

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
