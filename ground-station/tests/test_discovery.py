"""Unit tests for the pure parse helpers in app.gui.discovery.

These tests must run without a display or network. The discovery module
depends on PyQt6 for its QThread subclasses, but the parse helpers
themselves are pure. We import them via the module; if PyQt6 is not
available in the test environment, the test is skipped.
"""
from __future__ import annotations

import unittest


try:
    from app.gui.discovery import (
        STATIC_ONBOARD_HOST_DEFAULT,
        PeerSightingThrottle,
        SentNonceRegistry,
        parse_gs_beacon,
        parse_onboard_announcement,
        probe_host_candidates,
    )
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


@unittest.skipUnless(_IMPORT_OK, f"discovery module unavailable: {_IMPORT_ERR}")
class CommandProbeHelperTests(unittest.TestCase):
    def test_candidates_append_static_default_and_dedupe(self) -> None:
        self.assertEqual(
            probe_host_candidates("", "169.254.10.11", "169.254.10.11"),
            ["169.254.10.11", STATIC_ONBOARD_HOST_DEFAULT],
        )

    def test_candidates_can_respect_explicit_host_only(self) -> None:
        self.assertEqual(
            probe_host_candidates("127.0.0.1", include_static=False),
            ["127.0.0.1"],
        )


@unittest.skipUnless(_IMPORT_OK, f"discovery module unavailable: {_IMPORT_ERR}")
class SentNonceRegistryTests(unittest.TestCase):
    """Broadcast loopback self-recognition: the listener drops GS_BEACONs
    whose nonce this process itself sent."""

    def test_recognises_sent_nonce(self) -> None:
        reg = SentNonceRegistry()
        reg.add("1724500000000")
        self.assertTrue(reg.was_sent("1724500000000"))
        self.assertFalse(reg.was_sent("1724500000001"))

    def test_bounded_capacity_evicts_oldest(self) -> None:
        reg = SentNonceRegistry(capacity=3)
        for n in ("a", "b", "c", "d"):
            reg.add(n)
        self.assertFalse(reg.was_sent("a"))
        for n in ("b", "c", "d"):
            self.assertTrue(reg.was_sent(n))

    def test_duplicate_add_is_idempotent(self) -> None:
        reg = SentNonceRegistry(capacity=2)
        reg.add("x")
        reg.add("x")
        reg.add("y")
        self.assertTrue(reg.was_sent("x"))
        self.assertTrue(reg.was_sent("y"))


@unittest.skipUnless(_IMPORT_OK, f"discovery module unavailable: {_IMPORT_ERR}")
class PeerSightingThrottleTests(unittest.TestCase):
    """One event-log line per peer per window, not one per 2 s beacon."""

    def test_first_sighting_emits(self) -> None:
        throttle = PeerSightingThrottle(reseen_s=300.0)
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=0.0))

    def test_unchanged_peer_suppressed_within_window(self) -> None:
        throttle = PeerSightingThrottle(reseen_s=300.0)
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=0.0))
        for t in (2.0, 4.0, 60.0, 299.9):
            self.assertFalse(throttle.should_emit("192.168.1.50", 100, now=t))

    def test_unchanged_peer_reported_after_window(self) -> None:
        throttle = PeerSightingThrottle(reseen_s=300.0)
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=0.0))
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=300.0))

    def test_priority_change_reports_immediately(self) -> None:
        throttle = PeerSightingThrottle(reseen_s=300.0)
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=0.0))
        self.assertTrue(throttle.should_emit("192.168.1.50", 200, now=2.0))

    def test_second_peer_reports_independently(self) -> None:
        # The old single-slot dedup reset on every alternation, so two
        # peers beaconing A,B,A,B flooded the log; each key throttles on
        # its own now.
        throttle = PeerSightingThrottle(reseen_s=300.0)
        self.assertTrue(throttle.should_emit("192.168.1.50", 100, now=0.0))
        self.assertTrue(throttle.should_emit("192.168.1.60", 100, now=1.0))
        self.assertFalse(throttle.should_emit("192.168.1.50", 100, now=2.0))
        self.assertFalse(throttle.should_emit("192.168.1.60", 100, now=3.0))


if __name__ == "__main__":
    unittest.main()
