"""Headless smoke test: build the main window and push a scripted telemetry
packet through it. Skips if PyQt6 isn't available or the platform lacks a
usable QPA plugin even with `QT_QPA_PLATFORM=offscreen`.
"""
from __future__ import annotations

import os
import socket
import sys
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def _pump_until(app, predicate, timeout_s: float = 3.0) -> bool:
    """Process the Qt event loop (so queued cross-thread signals get
    delivered) until `predicate()` is true or `timeout_s` elapses. Returns
    the final predicate value."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


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
            # SEQUENT_RTD's component-state row now lives on the Health tab
            # (panels_health.HealthPanel), labeled "SEQUENT_RTD (RTD HAT)"
            # rather than the bare component key ValuesPanel used to show.
            self.assertTrue(any("SEQUENT_RTD" in t for t in label_texts))
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
        Connection panel's Start button must reflect that once the
        receiver's TCP bind actually succeeds (reported asynchronously via
        `status_changed("listening")` from the receiver's QThread) instead
        of still inviting a click that just logs "already running"."""
        from app.gui.main_window import MainWindow

        win = MainWindow(bind="127.0.0.1", tel_port=44003, cmd_port=45003,
                         cmd_host="127.0.0.1", log_path=Path("logs/smoke_autostart.csv"),
                         firewall_check=False)
        try:
            btn = win._connection._start_btn
            bound = _pump_until(self._app, lambda: not btn.isEnabled())
            self.assertTrue(bound, "receiver never reported listening")
            self.assertFalse(btn.isEnabled())
            self.assertIn("running", btn.text().lower())
        finally:
            win.close()

    def test_connection_panel_recovers_after_autostart_bind_failure(self) -> None:
        """If the telemetry port is already taken, the receiver's bind
        fails inside its QThread (dispatch.py's `run()` catches it and
        emits `status_changed("failed")`). The Connection panel's button
        must NOT be left disabled and reading "Receiver running" over a
        dead receiver — it must re-enable with a start affordance so the
        operator can retry, which is the entire reason the button exists."""
        from app.gui.main_window import MainWindow

        port = 44006
        blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # SO_REUSEADDR alone does not reliably reserve a port against a
        # second bind on Windows (it lets a second SO_REUSEADDR socket bind
        # to the same address without error) — use the exclusive flag where
        # available so this test actually forces the collision it claims to.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        blocker.bind(("127.0.0.1", port))
        blocker.listen(1)
        try:
            win = MainWindow(bind="127.0.0.1", tel_port=port, cmd_port=45006,
                             cmd_host="127.0.0.1",
                             log_path=Path("logs/smoke_autostart_fail.csv"),
                             firewall_check=False)
            try:
                # Wait for the *actual* failure signal, not merely "the
                # button happens to be enabled" — with the auto-start fix,
                # the button starts enabled and is only ever disabled by a
                # real "listening"/"connected" state, so an unconditioned
                # isEnabled() check would pass even if "failed" handling
                # were deleted entirely. Gate on the status text instead,
                # which only ever says "failed" via the status_changed
                # signal this test exists to cover.
                reported_failed = _pump_until(
                    self._app,
                    lambda: "failed" in win._connection._status.text().lower(),
                )
                self.assertTrue(reported_failed, "receiver never reported failed")
                btn = win._connection._start_btn
                self.assertTrue(btn.isEnabled())
                self.assertNotIn("running", btn.text().lower())
                self.assertIsNone(
                    win._receiver,
                    "dead receiver must be cleared so a retry isn't blocked "
                    "by the already-running guard",
                )
            finally:
                win.close()
        finally:
            blocker.close()

    def test_start_telemetry_retry_succeeds_after_bind_failure(self) -> None:
        """End-to-end recovery: bind fails, the operator (or another
        auto-start) retries once the port frees up, and the retry must
        actually take -- covers the identity guard too, since the retry
        constructs a second TelemetryReceiver while the first (dead) one
        may still have signals in flight."""
        from app.gui.main_window import MainWindow

        port = 44009
        blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        blocker.bind(("127.0.0.1", port))
        blocker.listen(1)

        win = MainWindow(bind="127.0.0.1", tel_port=port, cmd_port=45009,
                         cmd_host="127.0.0.1",
                         log_path=Path("logs/smoke_retry.csv"),
                         firewall_check=False)
        try:
            reported_failed = _pump_until(
                self._app,
                lambda: "failed" in win._connection._status.text().lower(),
            )
            self.assertTrue(reported_failed, "receiver never reported failed")
            btn = win._connection._start_btn
            self.assertTrue(btn.isEnabled())

            # Free the port, then retry exactly the way the operator
            # would: click the button.
            blocker.close()
            btn.click()

            # Synchronous check, before any event pump: the click itself
            # must not claim "running". The honest path can only flip the
            # button via the receiver's queued "listening" signal, which
            # cannot have been delivered yet -- so an immediate flip here
            # is exactly the optimistic lie the single-writer rule bans.
            self.assertTrue(
                btn.isEnabled(),
                "the click alone must not claim 'running' -- only the "
                "receiver's own listening/connected signal may flip the "
                "button (single-writer rule)",
            )

            bound = _pump_until(self._app, lambda: not btn.isEnabled())
            self.assertTrue(bound, "retry receiver never reported listening")
            self.assertIn("running", btn.text().lower())
            self.assertIsNotNone(
                win._receiver,
                "a successful retry must leave the live receiver tracked",
            )
        finally:
            win.close()

    def test_stale_receiver_signal_does_not_clobber_live_receiver(self) -> None:
        """Deterministic version of the race the guard exists for: a
        signal arrives from a receiver that is no longer `self._receiver`
        (e.g. a superseded attempt whose queued "failed" was delivered
        late). This is synthesized directly rather than raced -- a real
        QObject.emit() from a distinct, unconnected TelemetryReceiver gives
        genuine Qt sender-tracking (self.sender() inside the slot) without
        depending on thread/event-loop timing, so the guard is exercised
        exactly as it runs in production, deterministically."""
        from app.gui.dispatch import TelemetryReceiver
        from app.gui.main_window import MainWindow

        win = MainWindow(bind="127.0.0.1", tel_port=44010, cmd_port=45010,
                         cmd_host="127.0.0.1",
                         log_path=Path("logs/smoke_stale.csv"),
                         firewall_check=False)
        try:
            listening = _pump_until(
                self._app, lambda: not win._connection._start_btn.isEnabled()
            )
            self.assertTrue(listening, "live receiver never reported listening")
            live_receiver = win._receiver
            self.assertIsNotNone(live_receiver)

            # A stand-in for a superseded receiver -- never started, only
            # used for its QObject identity so its signal has a genuine,
            # distinct sender.
            stale = TelemetryReceiver("127.0.0.1", 44010, win._logs)
            stale.status_changed.connect(win._on_receiver_status)
            stale.connection_changed.connect(win._on_connection_changed)
            try:
                stale.status_changed.emit("failed")
                stale.connection_changed.emit(True, "10.0.0.99:9999")

                self.assertIs(
                    win._receiver, live_receiver,
                    "stale signal must not null out the live receiver",
                )
                btn = win._connection._start_btn
                self.assertFalse(btn.isEnabled())
                self.assertIn("running", btn.text().lower())
                self.assertFalse(
                    win._link_ok,
                    "stale connection_changed(True, ...) from a superseded "
                    "receiver must be dropped, not allowed to flip "
                    "_link_ok -- the live receiver never actually accepted "
                    "a peer in this test, so _link_ok must still be False",
                )
            finally:
                stale.deleteLater()
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
