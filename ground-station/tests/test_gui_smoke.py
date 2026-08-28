"""Headless smoke test: build the console and push a scripted telemetry
packet through it. Skips if PyQt6 isn't available or the platform lacks a
usable QPA plugin even with `QT_QPA_PLATFORM=offscreen`.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import frame, make_window  # noqa: E402


class GuiSmoke(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as exc:
            raise unittest.SkipTest(f"no Qt platform: {exc}")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.win = make_window(Path(self._tmp.name))

    def tearDown(self) -> None:
        self.win.close()
        self._tmp.cleanup()

    def test_mainwindow_builds_and_handles_packets(self) -> None:
        from app.protocol import parse_telemetry_csv
        from PyQt6.QtWidgets import QPushButton
        # A legacy single-STEPPER frame with no CTRL block must still render.
        legacy = (
            "DATA,sess-smoke,3,2026-04-13T12:00:00Z,1,-25.0,180.0,0.5,"
            "-29.5,-29.7,-29.9,-30.0,-30.1,-30.2,-30.0,-29.8,"
            "HEATER_DUTY=0.10|0.20|0.30|0.40|0.50|0.60,"
            "RESISTANCE=10.0|10.1|10.2|10.3|10.4|10.5|-|-,"
            "PHASE=FLOAT,MODE=RUN,"
            "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|RESISTANCE_OK,"
            "STEPPER=pos:500|tgt:800|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:500|src:cmd:MOVE"
        )
        self.win._on_packet(parse_telemetry_csv(legacy))
        self.win._on_packet(parse_telemetry_csv(frame(seq=4)))
        self.assertIsInstance(self.win._plots.toggle_paused(), bool)
        labels = {b.text() for b in self.win.findChildren(QPushButton)}
        for expected in ("SET ZERO", "BEND", "STANDARD PULL", "CHECK", "ARM", "RADIO SILENCE",
                         "HEATERS OFF", "STOP MOTORS", "RUN CHECK ALL", "LOAD"):
            self.assertIn(expected, labels)
        for gone in ("FORCE START", "FORCE STOP", "ROTATE", "▶ Start Telemetry"):
            self.assertNotIn(gone, labels)
        self.assertEqual(self.win._top.mode_text(), "RUN")
        self.assertEqual(self.win._state.seq, 4)
        self.assertEqual(self.win._health.dot_color("SD"), "#2ecc71")

    def test_checkout_motor_rows_reflect_enable_and_zero(self) -> None:
        from app.protocol import parse_telemetry_csv
        self.win._link_ok = True
        self.win._on_packet(parse_telemetry_csv(frame(m0="en:1|ok:1|mv:0|hold:0|zeroed:1", m1="en:1|ok:1|mv:0|hold:0|zeroed:1")))
        self.assertEqual(self.win._checkout.row_color("m0"), "#2ecc71")
        self.assertEqual(self.win._checkout.row_color("m1"), "#2ecc71")
        self.win._on_packet(parse_telemetry_csv(frame(m1="en:1|ok:1|mv:0|hold:0|zeroed:0")))
        self.assertEqual(self.win._checkout.row_color("m1"), "#f39c12", "enabled but not zeroed is amber")
        self.win._on_packet(parse_telemetry_csv(frame(m1="en:0|ok:0|mv:0|hold:0|zeroed:0",
                                                      comps="DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:FAILED|PWM:OK")))
        self.assertEqual(self.win._checkout.row_color("m1"), "#e74c3c")

    def test_no_debug_arm_controls(self) -> None:
        from PyQt6.QtWidgets import QPushButton
        labels = {b.text() for b in self.win.findChildren(QPushButton)}
        self.assertFalse(any("DEBUG" in label for label in labels), labels)

    def test_command_dispatcher_uses_static_host_for_blank_endpoint(self) -> None:
        from app.gui.dispatch import CommandDispatcher, DEFAULT_COMMAND_HOST
        dispatcher = CommandDispatcher("", 5000)
        self.assertEqual(dispatcher.host, DEFAULT_COMMAND_HOST)
        dispatcher.set_endpoint("   ", 5001)
        self.assertEqual(dispatcher.host, DEFAULT_COMMAND_HOST)
        self.assertEqual(dispatcher.port, 5001)

    def test_sample_buttons_carry_sends_tooltips(self) -> None:
        checks = [
            (self.win._system.btn_ping, "Sends: PING"),
            (self.win._system.btn_status, "Sends: STATUS"),
            (self.win._system.btn_check, "Sends: CHECK"),
            (self.win._system.btn_arm, "Sends: ARM"),
            (self.win._motion.btn_enable, "Sends: STEPPER_ENABLE <motor_id>"),
            (self.win._motion.btn_zero, "Sends: SET_POSITION_ZERO <motor_id>"),
            (self.win._motion.btn_bend, "Sends: STEPPER_MOVETO <motor_id> <target> <hold_s>"),
            (self.win._top.btn_heaters_off, "Sends: HEATERS_OFF"),
            (self.win._top.btn_stop_motors, "Sends: STEPPER_STOP 0"),
        ]
        for btn, expected in checks:
            self.assertIn(expected, btn.toolTip(), f"{btn.text()!r} tooltip {btn.toolTip()!r}")


if __name__ == "__main__":
    unittest.main()
