"""Layout contract (redesign spec §4): the console fits its minimum
supported screen with nothing clipped, named splitters persist, the status
bar tells the truth, and the receiver-state plumbing survived the rewrite."""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import frame, make_window, pump_until  # noqa: E402


class LayoutTests(unittest.TestCase):
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

    def test_fits_minimum_and_comfortable_screens(self) -> None:
        from app.protocol import parse_telemetry_csv
        self.win._on_packet(parse_telemetry_csv(frame()))
        self.win.show(); self._app.processEvents()
        for w, h in ((1366, 768), (1920, 1080)):
            self.win.resize(w, h); self._app.processEvents()
            self.assertEqual((self.win.width(), self.win.height()), (w, h),
                             f"window minimum {self.win.minimumSizeHint()} exceeds {w}x{h}")
            for name, tab in (("System", self.win._system), ("Thermal", self.win._thermal),
                              ("Motion", self.win._motion), ("Advanced", self.win._advanced)):
                self.win._left_tabs.setCurrentWidget(tab); self._app.processEvents()
                inner = tab.widget().minimumSizeHint().width()
                self.assertLessEqual(inner, tab.viewport().width(),
                                     f"{name} tab content ({inner} px) is wider than its column at {w}x{h} -- it would clip")
            self.assertLessEqual(self.win._top.minimumSizeHint().width(), w)
        self.win.hide()

    # MUTATION: give a label in tab_motion a setMinimumWidth(700) and confirm
    # test_fits_minimum_and_comfortable_screens names the Motion tab.

    def test_splitters_are_named_and_persist(self) -> None:
        for splitter in (self.win._main_splitter, self.win._body_splitter, self.win._bottom):
            self.assertTrue(splitter.objectName(), "unnamed splitters cannot be restored")
        self.win.resize(1600, 900); self.win.show(); self._app.processEvents()
        self.win._main_splitter.setSizes([520, 700, 300]); self._app.processEvents()
        before = self.win._main_splitter.sizes()
        state = self.win._main_splitter.saveState()
        self.win._main_splitter.setSizes([410, 800, 310]); self._app.processEvents()
        self.assertNotEqual(self.win._main_splitter.sizes(), before, "the second setSizes must actually move the handles")
        self.assertTrue(self.win._main_splitter.restoreState(state))
        self._app.processEvents()
        self.assertEqual(self.win._main_splitter.sizes(), before)
        self.win.hide()

    def test_status_bar_never_claims_start_telemetry(self) -> None:
        self.assertNotIn("start telemetry", self.win.statusBar().currentMessage().lower())
        self.assertIn("no session yet", self.win.statusBar().currentMessage())

    def test_receiver_autostart_and_failure_recovery(self) -> None:
        import socket
        listening = pump_until(self._app, lambda: self.win._receiver_state == "listening")
        self.assertTrue(listening, "receiver never reported listening")
        self.assertFalse(self.win._system.btn_restart_receiver.isVisibleTo(self.win._system))
        live = self.win._receiver
        # Failure: the restart control appears, link state is dropped.
        self.win._link_ok = True
        live.status_changed.emit("failed")
        self.assertIsNone(self.win._receiver)
        self.assertFalse(self.win._link_ok)
        self.assertTrue(self.win._system.btn_restart_receiver.isVisibleTo(self.win._system))
        self.assertEqual(self.win._checkout.row_color("link"), "#e74c3c")
        live.stop(); live.wait(2000)

    def test_stale_receiver_signal_is_ignored(self) -> None:
        from app.gui.dispatch import TelemetryReceiver
        listening = pump_until(self._app, lambda: self.win._receiver_state == "listening")
        self.assertTrue(listening)
        live = self.win._receiver
        stale = TelemetryReceiver("127.0.0.1", 1, self.win._logs)
        stale.status_changed.connect(self.win._on_receiver_status)
        stale.connection_changed.connect(self.win._on_connection_changed)
        try:
            stale.status_changed.emit("failed")
            stale.connection_changed.emit(True, "10.0.0.99:9999")
            self.assertIs(self.win._receiver, live, "stale signal must not null out the live receiver")
            self.assertFalse(self.win._link_ok)
        finally:
            stale.deleteLater()


if __name__ == "__main__":
    unittest.main()
