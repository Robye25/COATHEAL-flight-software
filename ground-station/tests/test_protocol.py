import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import (
    PullEvent, StepperSnapshot, TelemetryParseError, build_ack,
    parse_command_response, parse_pull_event, parse_telemetry_csv,
    validate_duty, validate_heater_index, validate_microstep,
    validate_pid_gains, validate_revolutions, validate_speed_hz,
    validate_stepper_move, validate_temperature_target,
    validate_tick_hz,
)


# Rev-B.1 baseline frame (no RESISTANCE= segment). The ground parser must
# accept this so older pre-resistance logs keep replaying.
LEGACY_DATA = (
    "DATA,session-1,1,2026-03-31T12:00:00Z,1,-30.00,150.00,1.23,"
    "-30.00,-30.10,-29.90,HEATER_DUTY=0.5|0.5|0.0,"
    "PHASE=ASCENT,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK"
)

# Rev-B.1 single-STEPPER legacy frame. `packet.steppers` should be a
# 1-element list mirroring `packet.stepper`.
STEPPER_DATA = (
    "DATA,sess-2,7,2026-04-13T01:02:03Z,1,-25.00,180.00,0.80,"
    "-30.00,-29.80,-29.90,HEATER_DUTY=0.10|0.20|0.30,"
    "PHASE=ASCENT,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK,"
    "STEPPER=pos:1234|tgt:2000|hz:800|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:1234|src:cmd:MOVE"
)

# Rev-B.1 production frame: 8 sample temps, 6 heater duties, RESISTANCE=
# with a mix of real values and '-' for the two unmeasured samples,
# dual-motor STEPPER0/STEPPER1, RESISTANCE_OK in STATUS.
DUAL_STEPPER_DATA = (
    "DATA,sess-b,42,2026-04-16T10:20:30Z,1,-10.00,140.00,0.01,"
    "-5.00,-5.10,-5.20,-5.30,-5.40,-5.50,-5.60,-5.70,"
    "HEATER_DUTY=0.00|0.10|0.20|0.30|0.40|0.50,"
    "RESISTANCE=10.5|11.0|9.8|10.1|10.7|10.3|-|-,"
    "PHASE=FLOAT,MODE=RUN,"
    "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK"
    "|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_INHIBITED|RESISTANCE_OK,"
    "STEPPER0=pos:100|tgt:200|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:100|src:cmd:MOVE,"
    "STEPPER1=pos:-50|tgt:-50|hz:200|us:8|en:1|mv:0|hold:1|hold_s:3.5|pulses:50|src:phase:FLOAT"
)

HEALTH_DATA = (
    "DATA,sess-health,9,2026-07-09T01:00:00Z,1,nan,100.0,nan,"
    "nan,21.5,nan,nan,nan,nan,nan,nan,"
    "HEATER_DUTY=0|0|0|0|0|0,RESISTANCE=-|-|-|-|-|-|-|-,"
    "PHASE=ASCENT,MODE=RUN,STATUS=I2C_FAIL|RS485_OK,"
    "SENSOR_VALID=AT:0|AP:1|UV:0|S0:0|S1:1|S2:0|S3:0|S4:0|S5:0|S6:0|S7:0,"
    "SENSOR_AGE_MS=AT:-1|AP:20|UV:-1|S0:-1|S1:125|S2:-1|S3:-1|S4:-1|S5:-1|S6:-1|S7:-1,"
    "COMPONENT_STATE=DPS310:DEGRADED|ADS1115:FAILED|DAQ132M:DEGRADED|"
    "RTD_CLICK:OK|MOTOR0:OK|MOTOR1:FAILED|PWM:DEGRADED"
)

RTD_CLICK_S1_DATA = (
    "DATA,sess-rtd,11,2026-07-10T01:30:00Z,1,22.0,1000.0,nan,"
    "nan,23.42,nan,nan,nan,nan,nan,nan,"
    "HEATER_DUTY=0|0|0|0|0|0,RESISTANCE=-|-|-|-|-|-|-|-,"
    "PHASE=BOOT,MODE=STANDBY,STATUS=I2C_OK|SPI_OK|SAMPLE_TEMP_OK,"
    "SENSOR_VALID=AT:1|AP:1|UV:0|S0:0|S1:1|S2:0|S3:0|S4:0|S5:0|S6:0|S7:0,"
    "SENSOR_AGE_MS=AT:40|AP:40|UV:-1|S0:-1|S1:50|S2:-1|S3:-1|S4:-1|S5:-1|S6:-1|S7:-1,"
    "COMPONENT_STATE=DPS310:OK|ADS1115:DISABLED|DAQ132M:DISABLED|"
    "RTD_CLICK:OK|MOTOR0:FAILED|MOTOR1:FAILED|PWM:OK"
)


class DataFrameTests(unittest.TestCase):
    def test_parse_valid_packet(self) -> None:
        pkt = parse_telemetry_csv(LEGACY_DATA)
        self.assertEqual(pkt.session_id, "session-1")
        self.assertEqual(pkt.seq, 1)
        self.assertEqual(pkt.phase, "ASCENT")
        self.assertEqual(len(pkt.heater_duty), 3)
        self.assertIsNone(pkt.stepper)
        self.assertEqual(pkt.steppers, [])
        self.assertEqual(pkt.mode, "")
        # Rev-B.1: missing RESISTANCE= segment surfaces as empty list.
        self.assertEqual(pkt.sample_resistance_ohm, [])

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

    # ── Rev-B.1: dual stepper + 8-sample + resistance frame ──────────────
    def test_parse_dual_stepper_frame(self) -> None:
        pkt = parse_telemetry_csv(DUAL_STEPPER_DATA)
        self.assertEqual(pkt.session_id, "sess-b")
        self.assertEqual(pkt.seq, 42)
        self.assertEqual(pkt.phase, "FLOAT")
        self.assertEqual(len(pkt.sample_temps_c), 8)
        self.assertEqual(len(pkt.heater_duty), 6)
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
        # Rev-B.1 STATUS bits are in the status string verbatim.
        self.assertIn("RS485_OK", pkt.status)
        self.assertIn("HEATER_INHIBITED", pkt.status)
        self.assertIn("RESISTANCE_OK", pkt.status)

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

    def test_parse_component_health_and_partial_sensor_validity(self) -> None:
        pkt = parse_telemetry_csv(HEALTH_DATA)
        self.assertFalse(pkt.sensor_valid["AT"])
        self.assertTrue(pkt.sensor_valid["S1"])
        self.assertEqual(pkt.sensor_age_ms["S1"], 125)
        self.assertEqual(pkt.component_state["DAQ132M"], "DEGRADED")
        self.assertEqual(pkt.component_state["RTD_CLICK"], "OK")
        self.assertEqual(pkt.component_state["PWM"], "DEGRADED")
        self.assertTrue(math.isnan(pkt.sample_temps_c[0]))
        self.assertAlmostEqual(pkt.sample_temps_c[1], 21.5)

    def test_legacy_frame_defaults_sensor_values_to_valid(self) -> None:
        pkt = parse_telemetry_csv(LEGACY_DATA)
        self.assertTrue(pkt.sensor_valid["AT"])
        self.assertTrue(pkt.sensor_valid["S0"])
        self.assertEqual(pkt.sensor_age_ms, {})
        self.assertEqual(pkt.component_state, {})

    def test_parse_single_rtd_click_pt100_on_s1(self) -> None:
        pkt = parse_telemetry_csv(RTD_CLICK_S1_DATA)
        self.assertEqual(len(pkt.sample_temps_c), 8)
        self.assertTrue(math.isnan(pkt.sample_temps_c[0]))
        self.assertAlmostEqual(pkt.sample_temps_c[1], 23.42)
        self.assertFalse(pkt.sensor_valid["S0"])
        self.assertTrue(pkt.sensor_valid["S1"])
        self.assertEqual(pkt.sensor_age_ms["S1"], 50)
        self.assertEqual(pkt.component_state["RTD_CLICK"], "OK")
        self.assertEqual(pkt.component_state["PWM"], "OK")


class ResistanceTests(unittest.TestCase):
    """Rev-B.1: the `RESISTANCE=` segment + `RESISTANCE_OK` STATUS bit."""

    def test_parse_resistance_happy_path(self) -> None:
        pkt = parse_telemetry_csv(DUAL_STEPPER_DATA)
        self.assertEqual(len(pkt.sample_resistance_ohm), 8)
        # First six channels are measured; values are floats.
        for i, expected in enumerate([10.5, 11.0, 9.8, 10.1, 10.7, 10.3]):
            self.assertAlmostEqual(pkt.sample_resistance_ohm[i], expected)
        # Last two are '-' on the wire → None in the packet.
        self.assertIsNone(pkt.sample_resistance_ohm[6])
        self.assertIsNone(pkt.sample_resistance_ohm[7])

    def test_parse_resistance_all_dashes(self) -> None:
        # All channels unmeasured: 8 '-' entries → list of 8 Nones.
        line = DUAL_STEPPER_DATA.replace(
            "RESISTANCE=10.5|11.0|9.8|10.1|10.7|10.3|-|-",
            "RESISTANCE=-|-|-|-|-|-|-|-",
        )
        pkt = parse_telemetry_csv(line)
        self.assertEqual(len(pkt.sample_resistance_ohm), 8)
        self.assertTrue(all(v is None for v in pkt.sample_resistance_ohm))

    def test_missing_resistance_segment_back_compat(self) -> None:
        # Onboard pre-RESISTANCE build: segment absent entirely. Parser
        # must still succeed and expose an empty list.
        self.assertEqual(parse_telemetry_csv(LEGACY_DATA).sample_resistance_ohm, [])

    def test_resistance_ok_status_bit(self) -> None:
        pkt = parse_telemetry_csv(DUAL_STEPPER_DATA)
        self.assertIn("RESISTANCE_OK", pkt.status)
        # RESISTANCE_FAIL variant: flip the bit and re-parse.
        fail_line = DUAL_STEPPER_DATA.replace("RESISTANCE_OK", "RESISTANCE_FAIL")
        pkt_fail = parse_telemetry_csv(fail_line)
        self.assertIn("RESISTANCE_FAIL", pkt_fail.status)
        self.assertNotIn("RESISTANCE_OK", pkt_fail.status)

    def test_invalid_resistance_rejected(self) -> None:
        # Non-numeric, non-dash value must raise.
        line = DUAL_STEPPER_DATA.replace(
            "RESISTANCE=10.5|11.0", "RESISTANCE=10.5|nope"
        )
        with self.assertRaises(TelemetryParseError):
            parse_telemetry_csv(line)


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
        # Rev-B.1: 6 heater channels (0..5). No box heater.
        self.assertTrue(validate_heater_index(0)[0])
        self.assertTrue(validate_heater_index(5)[0])
        self.assertFalse(validate_heater_index(6)[0])
        self.assertFalse(validate_heater_index(-1)[0])
        # Explicit count still honoured for callers that pre-set it.
        self.assertTrue(validate_heater_index(6, count=8)[0])

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
        for n in (1, 2, 4, 8, 16, 32, 64, 128, 256):
            self.assertTrue(validate_microstep(n)[0], msg=f"{n}")
        for n in (0, 3, 512):
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

    def test_temperature_target(self) -> None:
        self.assertTrue(validate_temperature_target(0.0)[0])
        self.assertTrue(validate_temperature_target(80.0)[0])
        self.assertFalse(validate_temperature_target(-0.1)[0])
        self.assertFalse(validate_temperature_target(80.1)[0])

    def test_pid_gains(self) -> None:
        self.assertTrue(validate_pid_gains(0.2, 0.02, 0.03)[0])
        self.assertTrue(validate_pid_gains(0.0, 0.0, 0.0)[0])
        self.assertFalse(validate_pid_gains(-0.1, 0.0, 0.0)[0])
        self.assertFalse(validate_pid_gains("x", 0.0, 0.0)[0])


if __name__ == "__main__":
    unittest.main()
