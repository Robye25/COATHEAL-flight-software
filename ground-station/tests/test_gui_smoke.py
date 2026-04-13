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

        win = MainWindow(bind="127.0.0.1", tel_port=44000, cmd_port=45000,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke.csv"),
                         firewall_check=False)
        try:
            line = (
                "DATA,sess-smoke,3,2026-04-13T12:00:00Z,1,-25.0,180.0,20.0,0.5,4.0,"
                "-29.5,-29.7,-29.9,-30.0,-30.1,-30.2,-30.0,-29.8,-29.6,"
                "HEATER_DUTY=0.10|0.20|0.30|0.40|0.50|0.60|0.70|0.80|0.90|0.15,"
                "PHASE=FLOAT_HOLD_+70C,MODE=RUN,"
                "STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK,"
                "STEPPER=pos:500|tgt:800|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:500|src:phase:FLOAT_HOLD"
            )
            pkt = parse_telemetry_csv(line)
            # Should not raise.
            win._on_packet(pkt)
            # Pause toggle should flip.
            self.assertIsInstance(win._plots.toggle_paused(), bool)
        finally:
            win.close()


if __name__ == "__main__":
    unittest.main()
