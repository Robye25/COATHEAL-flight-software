import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import TelemetryParseError, build_ack, parse_telemetry_csv


class ProtocolTests(unittest.TestCase):
    def test_parse_valid_packet(self) -> None:
        line = (
            "DATA,session-1,1,2026-03-31T12:00:00Z,1,-30.00,150.00,20.00,1.23,5.00,"
            "-30.00,-30.10,-29.90,HEATER_DUTY=0.5|0.5|0.0,"
            "PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK"
        )
        packet = parse_telemetry_csv(line)
        self.assertEqual(packet.session_id, "session-1")
        self.assertEqual(packet.seq, 1)
        self.assertEqual(packet.phase, "ASCENT_HOLD_-30C")
        self.assertEqual(len(packet.heater_duty), 3)

    def test_parse_invalid_packet(self) -> None:
        with self.assertRaises(TelemetryParseError):
            parse_telemetry_csv("broken")

    def test_build_ack(self) -> None:
        self.assertEqual(build_ack("session-1", 42), "ACK,session-1,42\n")


if __name__ == "__main__":
    unittest.main()
