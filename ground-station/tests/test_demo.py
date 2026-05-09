import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.demo import DemoTelemetryScenario, demo_command_response  # noqa: E402


class DemoTelemetryTests(unittest.TestCase):
    def test_demo_packet_has_full_ground_station_shape(self) -> None:
        scenario = DemoTelemetryScenario(tick_hz=2.0, speed=2.0)
        pkt = scenario.next_packet()

        self.assertEqual(pkt.session_id, "demo-stand")
        self.assertGreater(pkt.seq, 0)
        self.assertEqual(len(pkt.sample_temps_c), 8)
        self.assertEqual(len(pkt.heater_duty), 6)
        self.assertEqual(len(pkt.sample_resistance_ohm), 8)
        self.assertIsNone(pkt.sample_resistance_ohm[6])
        self.assertIsNone(pkt.sample_resistance_ohm[7])
        self.assertEqual(len(pkt.steppers), 2)
        self.assertIsNotNone(pkt.stepper)
        self.assertIn(pkt.phase, {"ASCENT", "FLOAT", "DESCENT", "LANDED"})
        self.assertIn("DEMO_SOURCE", pkt.status)

    def test_demo_emits_pull_events_during_float(self) -> None:
        scenario = DemoTelemetryScenario(tick_hz=2.0, speed=4.0)
        events = []
        for _ in range(40):
            scenario.next_packet()
            events.extend(scenario.drain_pull_events())

        self.assertTrue(events, "demo should emit at least one pull event")
        first = events[0]
        self.assertEqual(first.session_id, "demo-stand")
        self.assertIn(first.motor_id, {0, 1})
        self.assertTrue(first.samples)
        self.assertEqual(abs(first.steps_moved), scenario.PULL_STEPS)

    def test_demo_command_responses_are_ack_like(self) -> None:
        name, body = demo_command_response("PING")
        self.assertEqual(name, "PING")
        self.assertIn("pong", body)

        name, body = demo_command_response("SET_ALL_DUTY 0.25")
        self.assertEqual(name, "SET_ALL_DUTY")
        self.assertIn("accepted", body)


if __name__ == "__main__":
    unittest.main()
