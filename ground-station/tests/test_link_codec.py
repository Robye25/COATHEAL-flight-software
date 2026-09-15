"""z1 telemetry compression (docs/link-budget.md, "Telemetry framing"): the
preset dictionary, HELLO negotiation, and the `Z1,<base64>` line codec,
checked against the frame the onboard's own codec test pins. No Qt."""
from __future__ import annotations

import sys
import unittest
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import link_codec  # noqa: E402
from app.link_codec import CodecError, decode_line, encode_line, hello_reply  # noqa: E402
from app.protocol import parse_pull_event, parse_telemetry_csv  # noqa: E402

# The DATA line tests/unit/test_link_budget.cpp encodes onboard, and the wire
# form it pins (encoded with Python's zlib, same parameters).
ONBOARD_LINE = (
    "DATA,coatheal-1789498045-582267,547,2026-09-15T18:48:05Z,1,25.36,1007.96,0.00,26.10,24.29,"
    "24.33,24.37,24.01,24.51,24.25,24.06,HEATER_DUTY=1.000|1.000|1.000|0.000|0.000|0.000,"
    "RESISTANCE=-|-|-|-|-|-|-|-,PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|"
    "LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_FAIL|OVERTEMP_OK|ENERGY_OK|PWM_OK|"
    "STEPPER_OK|SAMPLE_TEMP_OK|REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK,"
    "SENSOR_VALID=AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1,"
    "SENSOR_AGE_MS=AT:692|AP:692|UV:430|S0:698|S1:698|S2:698|S3:698|S4:698|S5:698|S6:698|"
    "S7:698,COMPONENT_STATE=DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK,"
    "CTRL=fallback:0|link_loss_s:0.0|energy_wh:1.02|budget_wh:130.0|budget_exhausted:0|"
    "heaters_active:3|queue:0|plan:none|debug:0|tune:-,STEPPER0=pos:0|tgt:0|hz:50.00|us:4|"
    "ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:4800|missed:0|src:cmd:BEND_MM|zeroed:1|seq:-|"
    "seqst:idle|amps:0.00|acc:200.0|mm:0.000|mm_tgt:0.000|therm:ok,STEPPER1=pos:800|tgt:800|"
    "hz:50.00|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:4000|missed:0|src:cmd:BEND_MM|"
    "zeroed:1|seq:-|seqst:idle|amps:0.00|acc:200.0|mm:2.000|mm_tgt:2.000|therm:ok,TX=0")
ONBOARD_WIRE = (
    "Z1,1dZNC8IwDAbgP6SSpk3W7uZQT26C4NmDXv3/V5e0w9UPHGUIXvpCGWkOy0Pe+BM8OFqSR+RqQa7K+PG18zVQ4scO/HD"
    "ih1cGHpuOtXrq7gPKD0WESG8448doc+OzgJ/d/rBO/hxP3VR7+pwkjxr15/ZwQCkj0RdyFqQUB115NDCGjeFiUAyOUUn8Uh"
    "zIuOn/DizgxiZuYBZrzLfxdR6eJvhyu9bNttuc23YYZPNxkKFUHXlWvpEsbR1mbR3HreOrQXc=")


def other_frame(line: str = ONBOARD_LINE) -> str:
    """The same frame from another onboard session with other readings."""
    return (line.replace("coatheal-1789498045-582267,547,2026-09-15T18:48:05Z",
                         "coatheal-1790012345-424242,31337,2026-09-16T08:03:11Z")
                .replace("25.36,1007.96,0.00,26.10,24.29,24.33,24.37,24.01,24.51,24.25,24.06",
                         "-41.37,87.52,0.12,63.08,61.95,58.44,22.10,70.02,69.87,65.31,-12.40")
                .replace("HEATER_DUTY=1.000|1.000|1.000", "HEATER_DUTY=0.412|0.388|0.951")
                .replace("energy_wh:1.02", "energy_wh:42.17").replace("queue:0", "queue:12")
                .replace(",TX=0", ",TX=3"))


class DictionaryTests(unittest.TestCase):
    def test_dictionary_is_the_protocol_file(self) -> None:
        self.assertTrue(link_codec.available())
        self.assertEqual(link_codec.DICTIONARY_PATH.parent.name, "protocol")
        self.assertEqual(len(link_codec.DICTIONARY), 2375)
        self.assertEqual(link_codec.DICTIONARY_CRC, "85904299")
        self.assertEqual(link_codec.DICTIONARY_CRC, f"{zlib.crc32(link_codec.DICTIONARY):08x}")

    def test_missing_file_is_no_dictionary(self) -> None:
        self.assertIsNone(link_codec._load_dictionary(Path(__file__).with_name("no-such-dictionary.txt")))


class HelloTests(unittest.TestCase):
    def test_matching_crc_selects_z1(self) -> None:
        self.assertEqual(hello_reply("HELLO,coatheal-1789498045-582267,z1:85904299"), "HELLO,z1\n")
        self.assertEqual(hello_reply("HELLO,s,z1:85904299\n"), "HELLO,z1\n")
        self.assertEqual(hello_reply("HELLO,s,lz4:0badc0de;z1:85904299"), "HELLO,z1\n", "';'-separated offers")
        self.assertEqual(hello_reply("HELLO,s,lz4:0badc0de,z1:85904299"), "HELLO,z1\n", "','-separated offers")

    def test_anything_else_stays_plain(self) -> None:
        self.assertEqual(hello_reply("HELLO,s,z1:85904298"), "HELLO,plain\n", "a different dictionary")
        self.assertEqual(hello_reply("HELLO,s,z2:85904299"), "HELLO,plain\n")
        self.assertEqual(hello_reply("HELLO,s"), "HELLO,plain\n")
        self.assertEqual(hello_reply("HELLO,s,"), "HELLO,plain\n")

    def test_no_dictionary_never_offers_z1(self) -> None:
        with mock.patch.object(link_codec, "DICTIONARY", None), mock.patch.object(link_codec, "DICTIONARY_CRC", None):
            self.assertFalse(link_codec.available())
            self.assertEqual(hello_reply("HELLO,s,z1:85904299"), "HELLO,plain\n")
            with self.assertRaises(CodecError):
                decode_line(ONBOARD_WIRE)

    def test_other_lines_are_not_hellos(self) -> None:
        for line in ("HELLO", "HELLOZ,s,z1:85904299", "DATA,s,1", "Z1,AAAA", "", "ACK,s,1"):
            self.assertIsNone(hello_reply(line), line)

    def test_a_turned_down_z1_offer_is_logged_as_a_warning(self) -> None:
        # The events panel shows a line containing "warn" at WARN level.
        ok = link_codec.describe_hello("HELLO,s,z1:85904299", "HELLO,z1\n")
        self.assertIn("HELLO,z1", ok)
        self.assertNotIn("warn", ok.lower())
        mismatch = link_codec.describe_hello("HELLO,s,z1:0badc0de", "HELLO,plain\n")
        self.assertIn("85904299", mismatch)
        self.assertTrue(mismatch.startswith("WARNING:"), mismatch)
        self.assertNotIn("warn", link_codec.describe_hello("HELLO,s", "HELLO,plain\n").lower(),
                         "an onboard that offers no codec is not a problem")
        with mock.patch.object(link_codec, "DICTIONARY_CRC", None):
            self.assertIn("no dictionary", link_codec.describe_hello("HELLO,s,z1:85904299", "HELLO,plain\n"))

    # MUTATION: return "HELLO,z1\n" from hello_reply whenever a z1 offer is
    # present (skip the CRC comparison) and confirm test_anything_else_stays_plain fails.


class LineCodecTests(unittest.TestCase):
    def test_decodes_the_onboard_wire_form(self) -> None:
        self.assertEqual(decode_line(ONBOARD_WIRE), ONBOARD_LINE)
        self.assertEqual(encode_line(ONBOARD_LINE), ONBOARD_WIRE, "same zlib parameters, same bytes")

    def test_round_trip_of_a_different_session(self) -> None:
        line = other_frame()
        self.assertNotEqual(line, ONBOARD_LINE)
        wire = encode_line(line)
        self.assertTrue(wire.startswith("Z1,"))
        decoded = decode_line(wire)
        self.assertEqual(decoded, line)
        pkt = parse_telemetry_csv(decoded)
        self.assertEqual((pkt.session_id, pkt.seq), ("coatheal-1790012345-424242", 31337))
        self.assertEqual(pkt.sample_temps_c[0], 63.08)
        self.assertEqual(pkt.tx_age_s, 3.0)

    def test_a_realistic_data_line_fits_300_characters(self) -> None:
        for line in (ONBOARD_LINE, other_frame()):
            self.assertGreater(len(line), 1100)
            self.assertLessEqual(len(encode_line(line)), 300, line[:40])

    def test_event_lines(self) -> None:
        line = "EVT,PULL,coatheal-1790012345-424242,7,1,2026-09-16T08:03:15Z,1600,5.00,4|5|6|7,4"
        self.assertEqual(parse_pull_event(decode_line(encode_line(line))).steps_moved, 1600)

    def test_plain_lines_pass_through(self) -> None:
        for line in ("DATA,s,1,x", "ACK,s,1", "EVT,PULL,s", "z1,abc", "HELLO,s,z1:85904299"):
            self.assertEqual(decode_line(line), line)

    def test_bad_frames_raise_codec_error(self) -> None:
        good = encode_line(ONBOARD_LINE)
        for bad in ("Z1,@@@@", "Z1,abc", "Z1,", good[:-8], "Z1,AAAA", "Z1," + "A" * 40):
            with self.assertRaises(CodecError, msg=bad[:20]):
                decode_line(bad)
        self.assertTrue(issubclass(CodecError, ValueError))


if __name__ == "__main__":
    unittest.main()
