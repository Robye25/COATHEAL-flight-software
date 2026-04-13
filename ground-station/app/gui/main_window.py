"""Main window — assembles all panels, routes telemetry, wires shortcuts."""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

import pyqtgraph as pg
from PyQt6.QtCore import Qt, QSettings
from PyQt6.QtGui import QAction, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QApplication, QDockWidget, QMainWindow, QMessageBox, QStatusBar,
    QTabWidget, QToolBox, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, TelemetryPacket
from .dispatch import CommandDispatcher, TelemetryReceiver
from .panels_control import (
    CommandPanel, ConnectionPanel, EmergencyBar, HeaterPanel, ModePanel,
    StepperPanel,
)
from .panels_info import (
    CmdHistoryPanel, LogPanel, PreflightPanel, TopStatusStrip, ValuesPanel,
)
from .plots import PlotTabs
from .widgets import Toast


class MainWindow(QMainWindow):
    def __init__(self, *, bind: str, tel_port: int, cmd_port: int, cmd_host: str,
                 log_path: Path):
        super().__init__()
        self.setWindowTitle("COATHEAL Ground Station")
        self.resize(1500, 920)

        self._settings = QSettings("COATHEAL", "GroundStation")
        self._log_path = log_path
        self._receiver: Optional[TelemetryReceiver] = None
        self._link_ok = False

        self._dispatcher = CommandDispatcher(cmd_host, cmd_port)
        self._dispatcher.response_received.connect(self._on_response)

        pg.setConfigOption("background", "#0d0d0d")
        pg.setConfigOption("foreground", "#cccccc")

        # ── central: top status + plots ──
        central = QWidget()
        v = QVBoxLayout(central); v.setContentsMargins(0, 0, 0, 0); v.setSpacing(0)
        self._top = TopStatusStrip()
        self._plots = PlotTabs()
        v.addWidget(self._top); v.addWidget(self._plots, 1)
        self.setCentralWidget(central)

        # ── left dock: QToolBox ──
        self._connection = ConnectionPanel(bind, tel_port, cmd_port, cmd_host)
        self._connection.start_requested.connect(self._on_start_telemetry)
        self._mode_panel = ModePanel(self._dispatcher)
        self._heater_panel = HeaterPanel(self._dispatcher)
        self._stepper_panel = StepperPanel(self._dispatcher)
        self._command_panel = CommandPanel(self._dispatcher)
        self._command_panel.debug_armed_changed.connect(self._heater_panel.set_armed)

        left_box = QToolBox()
        left_box.addItem(self._connection,    "Connection")
        left_box.addItem(self._mode_panel,    "Mode / Phase")
        left_box.addItem(self._heater_panel,  "Heaters")
        left_box.addItem(self._stepper_panel, "Stepper")
        left_box.addItem(self._command_panel, "Diagnostics")

        left_dock = QDockWidget("Controls", self)
        left_dock.setWidget(left_box)
        left_dock.setAllowedAreas(Qt.DockWidgetArea.LeftDockWidgetArea | Qt.DockWidgetArea.RightDockWidgetArea)
        left_dock.setMinimumWidth(340)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, left_dock)

        # ── right dock: tabs ──
        self._values = ValuesPanel()
        self._preflight = PreflightPanel()
        self._history = CmdHistoryPanel()
        self._history.reissue_requested.connect(lambda cmd: self._dispatcher.send(cmd, tag=self._history))
        right_tabs = QTabWidget()
        right_tabs.addTab(self._values,    "Values")
        right_tabs.addTab(self._preflight, "Preflight")
        right_tabs.addTab(self._history,   "Cmd History")
        right_dock = QDockWidget("Status", self)
        right_dock.setWidget(right_tabs)
        right_dock.setMinimumWidth(280)
        self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, right_dock)

        # ── bottom: emergency bar + log ──
        bottom_container = QWidget()
        bv = QVBoxLayout(bottom_container); bv.setContentsMargins(0, 0, 0, 0); bv.setSpacing(0)
        self._emergency = EmergencyBar(self._dispatcher)
        self._log = LogPanel()
        bv.addWidget(self._emergency)
        bv.addWidget(self._log, 1)
        bottom_dock = QDockWidget("Emergency & Log", self)
        bottom_dock.setWidget(bottom_container)
        bottom_dock.setAllowedAreas(Qt.DockWidgetArea.BottomDockWidgetArea)
        bottom_dock.setMinimumHeight(180)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, bottom_dock)

        # ── status bar ──
        self.setStatusBar(QStatusBar())
        self.statusBar().showMessage("Ready — start telemetry to begin.")

        # ── menus + shortcuts ──
        self._build_menus()
        self._build_shortcuts()

        # ── restore geometry ──
        geo = self._settings.value("window/geometry")
        if geo:
            self.restoreGeometry(geo)
        state = self._settings.value("window/state")
        if state:
            self.restoreState(state)

    # ── telemetry plumbing ──
    def _on_start_telemetry(self, bind: str, tel_port: int, cmd_port: int, cmd_host: str) -> None:
        self._dispatcher.set_endpoint(cmd_host, cmd_port)
        self._receiver = TelemetryReceiver(bind, tel_port, self._log_path)
        self._receiver.packet_received.connect(self._on_packet)
        self._receiver.log_message.connect(self._log.append)
        self._receiver.connection_changed.connect(self._on_connection_changed)
        self._receiver.start()

    def _on_connection_changed(self, connected: bool, addr: str) -> None:
        self._link_ok = connected
        self._connection.set_connected(connected, addr)
        self.statusBar().showMessage(f"{'Connected: ' + addr if connected else 'Waiting for onboard…'}")

    def _on_packet(self, pkt: TelemetryPacket) -> None:
        self._top.on_packet(pkt)
        self._plots.on_packet(pkt)
        self._values.on_packet(pkt)
        self._heater_panel.update_from_packet(pkt)
        self._stepper_panel.update_from_packet(pkt)
        self._mode_panel.update_from_packet(pkt)
        self._preflight.on_packet(pkt, self._link_ok)

    def _on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        ok = resp.ok
        body = resp.body if ok else resp.error or resp.raw
        self._log.append(f"[{'ACK' if ok else 'NACK'}] {cmd}  ({ms:.0f} ms)  {body}")
        # Anchor toast to the originating widget when possible.
        try:
            if hasattr(tag, "isWidgetType") and tag.isWidgetType():
                Toast.anchor(tag, ("✔ " if ok else "✖ ") + (body or cmd), ok=ok)
            else:
                Toast.anchor(self, ("✔ " if ok else "✖ ") + (body or cmd), ok=ok)
        except RuntimeError:
            pass
        # Append to history.
        for entry in reversed(self._dispatcher.history()):
            if entry.command == cmd and entry.latency_ms == ms:
                self._history.append(entry)
                break

    # ── menus ──
    def _build_menus(self) -> None:
        file_menu = self.menuBar().addMenu("&File")
        act_clear = QAction("Clear plots", self); act_clear.triggered.connect(self._plots.clear)
        file_menu.addAction(act_clear)
        act_quit = QAction("Quit", self); act_quit.triggered.connect(self.close); act_quit.setShortcut("Ctrl+Q")
        file_menu.addAction(act_quit)

        help_menu = self.menuBar().addMenu("&Help")
        act_cheat = QAction("Keyboard shortcuts", self); act_cheat.setShortcut("F1")
        act_cheat.triggered.connect(self._show_cheatsheet)
        help_menu.addAction(act_cheat)

    def _show_cheatsheet(self) -> None:
        QMessageBox.information(
            self, "Shortcuts",
            "Space  — pause/resume plots\n"
            "Esc    — STEPPER_STOP\n"
            "Shift+H — HEATERS_OFF\n"
            "Shift+E — ENTER_SAFE (confirm)\n"
            "Shift+S — RADIO_SILENCE (confirm)\n"
            "Shift+R — RADIO_RESUME\n"
            "Ctrl+1..5 — switch plot tab\n"
            "Ctrl+Q — quit\n"
        )

    # ── shortcuts ──
    def _build_shortcuts(self) -> None:
        def sc(keys: str, slot, scope=Qt.ShortcutContext.ApplicationShortcut):
            s = QShortcut(QKeySequence(keys), self); s.setContext(scope); s.activated.connect(slot); return s

        sc("Space",   self._toggle_pause)
        sc("Esc",     self._stepper_panel.emergency_stop)
        sc("Shift+H", lambda: self._dispatcher.send("HEATERS_OFF",   tag=self._emergency))
        sc("Shift+E", lambda: self._fire_confirm("ENTER_SAFE",    "Enter SAFE?"))
        sc("Shift+S", lambda: self._fire_confirm("RADIO_SILENCE", "Silence downlink?"))
        sc("Shift+R", lambda: self._dispatcher.send("RADIO_RESUME", tag=self._emergency))
        for i in range(1, 6):
            sc(f"Ctrl+{i}", lambda idx=i - 1: self._plots.setCurrentIndex(idx))

    def _fire_confirm(self, cmd: str, body: str) -> None:
        from .widgets import confirm
        if confirm(self, cmd, body):
            self._dispatcher.send(cmd, tag=self._emergency)

    def _toggle_pause(self) -> None:
        paused = self._plots.toggle_paused()
        self.statusBar().showMessage("Plots paused (Space to resume)" if paused else "Plots live", 2000)

    # ── lifecycle ──
    def closeEvent(self, event) -> None:  # noqa: N802
        self._settings.setValue("window/geometry", self.saveGeometry())
        self._settings.setValue("window/state", self.saveState())
        if self._receiver is not None:
            self._receiver.stop()
            self._receiver.wait(2000)
        super().closeEvent(event)


def run_gui(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="COATHEAL Ground Station")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--tel-port", type=int, default=4000)
    parser.add_argument("--cmd-port", type=int, default=5000)
    parser.add_argument("--host", default="169.254.10.10")
    parser.add_argument("--log", type=Path, default=Path("logs/ground_telemetry.csv"))
    args = parser.parse_args(argv)

    app = QApplication.instance() or QApplication([])
    from .theme import apply_dark_palette
    apply_dark_palette(app)

    win = MainWindow(bind=args.bind, tel_port=args.tel_port,
                     cmd_port=args.cmd_port, cmd_host=args.host,
                     log_path=args.log)
    win.show()
    return app.exec()
