"""Unit tests for the pure parse helpers in app.gui.discovery.

These tests must run without a display or network. The discovery module
depends on PyQt6 for its QThread subclasses, but the parse helpers
themselves are pure. We import them via the module; if PyQt6 is not
available in the test environment, the test is skipped.
"""
from __future__ import annotations

import unittest


try:
    from app.gui.discovery import parse_gs_beacon, parse_onboard_announcement
    _IMPORT_OK = True
    _IMPORT_ERR = ""
except Exception as exc:  # pylint: disable=broad-except
    _IMPORT_OK = False
    _IMPORT_ERR = str(exc)


@unittest.skipUnless(_IMPORT_OK, f"discovery module unavailable: {_IMPORT_ERR}")
class GsBeaconParseTests(unittest.TestCase):
    def test_valid(self) -> None:
        d = parse_gs_beacon("GS_BEACON,123456,4000,5000,100")
        self.assertEqual(d, {"nonce": "123456", "tel_port": 4000,
                             "cmd_port": 5000, "priority": 100})

    def test_trailing_newline_and_spaces(self) -> None:
        d = parse_gs_beacon(" GS_BEACON , abc , 4000 , 5000 , 50 \n")
        self.assertIsNotNone(d)
        assert d is not None
        self.assertEqual(d["priority"], 50)
        self.assertEqual(d["nonce"], "abc")

    def test_wrong_tag(self) -> None:
        self.assertIsNone(parse_gs_beacon("ONBOARD_BEACON,a,b,1,2"))

    def test_too_few_fields(self) -> None:
        self.assertIsNone(parse_gs_beacon("GS_BEACON,1,2,3"))

    def test_non_numeric_port(self) -> None:
        self.assertIsNone(parse_gs_beacon("GS_BEACON,n,x,5000,100"))

    def test_empty(self) -> None:
        self.assertIsNone(parse_gs_beacon(""))
        self.assertIsNone(parse_gs_beacon(None))  # type: ignore[arg-type]


@unittest.skipUnless(_IMPORT_OK, f"discovery module unavailable: {_IMPORT_ERR}")
class OnboardAnnouncementParseTests(unittest.TestCase):
    def test_beacon(self) -> None:
        d = parse_onboard_announcement("ONBOARD_BEACON,sess-1,coatheal-pi,5000,4000")
        self.assertEqual(d, {
            "kind": "beacon", "session_id": "sess-1", "hostname": "coatheal-pi",
            "cmd_port": 5000, "tel_port": 4000, "nonce": None,
        })

    def test_hello_legacy(self) -> None:
        d = parse_onboard_announcement(
            "ONBOARD_HELLO,42,sess-2,pi.local,5000,4000"
        )
        assert d is not None
        self.assertEqual(d["kind"], "hello")
        self.assertEqual(d["nonce"], "42")
        self.assertEqual(d["session_id"], "sess-2")
        self.assertEqual(d["hostname"], "pi.local")
        self.assertEqual(d["cmd_port"], 5000)
        self.assertEqual(d["tel_port"], 4000)

    def test_unknown_tag(self) -> None:
        self.assertIsNone(parse_onboard_announcement("FOO,a,b,c,d"))

    def test_too_few_fields_beacon(self) -> None:
        self.assertIsNone(parse_onboard_announcement("ONBOARD_BEACON,sess,host,5000"))

    def test_too_few_fields_hello(self) -> None:
        self.assertIsNone(parse_onboard_announcement("ONBOARD_HELLO,1,2,3,4"))

    def test_bad_ports(self) -> None:
        self.assertIsNone(parse_onboard_announcement(
            "ONBOARD_BEACON,sess,host,five-thousand,4000"
        ))

    def test_empty(self) -> None:
        self.assertIsNone(parse_onboard_announcement(""))
        self.assertIsNone(parse_onboard_announcement(None))  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
