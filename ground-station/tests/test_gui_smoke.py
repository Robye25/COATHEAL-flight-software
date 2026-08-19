"""Headless smoke test: build the main window and push a scripted telemetry
packet through it. Skips if PyQt6 isn't available or the platform lacks a
usable QPA plugin even with `QT_QPA_PLATFORM=offscreen`.
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


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

    def test_mainwindow_builds_and_handles_packet(self) -> None:
        from app.gui.main_window import MainWindow
        from app.protocol import parse_telemetry_csv
        from PyQt6.QtWidgets import QLabel, QPushButton

        win = MainWindow(bind="127.0.0.1", tel_port=44000, cmd_port=45000,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke.csv"),
                         firewall_check=False)
        try:
            line = (
                "DATA,sess-smoke,3,2026-04-13T12:00:00Z,1,-25.0,180.0,0.5,"
                "-29.5,-29.7,-29.9,-30.0,-30.1,-30.2,-30.0,-29.8,"
                "HEATER_DUTY=0.10|0.20|0.30|0.40|0.50|0.60,"
                "RESISTANCE=10.0|10.1|10.2|10.3|10.4|10.5|-|-,"
                "PHASE=FLOAT,MODE=RUN,"
                "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|RESISTANCE_OK,"
                "STEPPER=pos:500|tgt:800|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:500|src:phase:FLOAT"
            )
            pkt = parse_telemetry_csv(line)
            # Should not raise.
            win._on_packet(pkt)
            # Pause toggle should flip.
            self.assertIsInstance(win._plots.toggle_paused(), bool)
            labels = {button.text() for button in win.findChildren(QPushButton)}
            self.assertIn("SET ZERO", labels)
            self.assertIn("LOAD", labels)
            self.assertIn("CHECK", labels)
            self.assertIn("COMPONENTS", labels)
            self.assertIn("Target", labels)
            label_texts = {label.text() for label in win.findChildren(QLabel)}
            self.assertIn("SEQUENT_RTD", label_texts)
            self.assertNotIn("BEND ASCENT", labels)
            self.assertNotIn("BEND FLOAT", labels)
            self.assertNotIn("BEND DESCENT", labels)
            self.assertNotIn("SECONDARY CYCLE", labels)
        finally:
            win.close()

    def test_preflight_stepper_dot_reflects_motor_enable_state(self) -> None:
        """The preflight "Motors enabled" dot must be derived from the
        dual-motor `pkt.steppers` list, not the legacy single-motor
        `pkt.stepper` field (which modern dual-motor frames never
        populate and which would leave this dot permanently red)."""
        from app.gui.main_window import MainWindow
        from app.protocol import TelemetryPacket

        win = MainWindow(bind="127.0.0.1", tel_port=44001, cmd_port=45001,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke_preflight.csv"),
                         firewall_check=False)
        try:
            def motor(motor_id: int, enabled: bool) -> dict:
                return {
                    "motor_id": motor_id, "position": 0, "target": 0,
                    "hz": 400.0, "microstep": 16, "enabled": enabled,
                    "healthy": True, "moving": False, "holding": False,
                    "hold_s": 0.0, "pulses": 0, "missed_deadlines": 0,
                    "source": "test",
                }

            def packet(steppers) -> TelemetryPacket:
                return TelemetryPacket(
                    session_id="sess-preflight", seq=1,
                    timestamp="2026-04-13T12:00:00Z", rtc_valid=1,
                    ambient_temp_c=-25.0, ambient_pressure_mbar=180.0,
                    uv=0.5, sample_temps_c=[-29.0] * 8, heater_duty=[0.1] * 6,
                    sample_resistance_ohm=[10.0] * 8, phase="FLOAT",
                    status="LINK_OK", mode="RUN", steppers=steppers,
                )

            win._on_packet(packet([motor(0, True), motor(1, True)]))
            self.assertEqual(win._preflight.dot_color("stepper_en"), "#2ecc71",
                              "both motors enabled should be green")

            win._on_packet(packet([motor(0, True), motor(1, False)]))
            self.assertEqual(win._preflight.dot_color("stepper_en"), "#f39c12",
                              "exactly one motor enabled should be amber")

            win._on_packet(packet([motor(0, False), motor(1, False)]))
            self.assertEqual(win._preflight.dot_color("stepper_en"), "#e74c3c",
                              "no motors enabled should be red")
        finally:
            win.close()

    def test_no_debug_armed_signal(self) -> None:
        """ARM_DEBUG/DISARM were removed as dedicated CommandPanel controls
        (owner ruling) — the bench command is still reachable through the
        free-command box. This documents that the gating machinery
        (`debug_armed_changed`, `HeaterPanel.set_armed`) is fully gone."""
        from app.gui.main_window import MainWindow

        win = MainWindow(bind="127.0.0.1", tel_port=44002, cmd_port=45002,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke_armdebug.csv"),
                         firewall_check=False)
        try:
            self.assertFalse(hasattr(win._command_panel, "debug_armed_changed"))
            self.assertFalse(hasattr(win._heater_panel, "set_armed"))
            self.assertFalse(hasattr(win._command_panel, "_on_arm_debug"))
            self.assertFalse(hasattr(win._command_panel, "_on_disarm_debug"))
        finally:
            win.close()

    def test_connection_panel_shows_running_after_autostart(self) -> None:
        """MainWindow auto-starts the telemetry receiver at boot; the
        Connection panel's Start button must reflect that immediately
        instead of still inviting a click that just logs "already
        running"."""
        from app.gui.main_window import MainWindow

        win = MainWindow(bind="127.0.0.1", tel_port=44003, cmd_port=45003,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke_autostart.csv"),
                         firewall_check=False)
        try:
            btn = win._connection._start_btn
            self.assertFalse(btn.isEnabled())
            self.assertIn("running", btn.text().lower())
        finally:
            win.close()

    def test_command_dispatcher_uses_static_host_for_blank_endpoint(self) -> None:
        from app.gui.dispatch import CommandDispatcher, DEFAULT_COMMAND_HOST

        dispatcher = CommandDispatcher("", 5000)
        self.assertEqual(dispatcher.host, DEFAULT_COMMAND_HOST)
        dispatcher.set_endpoint("   ", 5001)
        self.assertEqual(dispatcher.host, DEFAULT_COMMAND_HOST)
        self.assertEqual(dispatcher.port, 5001)


if __name__ == "__main__":
    unittest.main()
