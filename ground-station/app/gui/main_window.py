"""Main window — assembles all panels, routes telemetry, wires shortcuts."""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

import pyqtgraph as pg
from PyQt6.QtCore import Qt, QSettings
from PyQt6.QtGui import QAction, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QApplication, QDockWidget, QMainWindow, QMessageBox, QScrollArea,
    QSizePolicy, QSplitter, QStatusBar, QTabWidget, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, TelemetryPacket
from . import firewall
from .dispatch import CommandDispatcher, TelemetryReceiver
from .discovery import GsBeacon, OnboardListener, DISCOVERY_PORT_DEFAULT
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
                 log_path: Path, firewall_check: bool = True):
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

        # ── left dock: all panels stacked in a resizable splitter, scroll
        # vertically if the window is short. Each panel is a QGroupBox with
        # a size policy that lets the user drag the splitter to re-balance
        # heights. Horizontal scroll is never needed — panels wrap to the
        # dock width.
        self._connection = ConnectionPanel(bind, tel_port, cmd_port, cmd_host)
        self._connection.start_requested.connect(self._on_start_telemetry)
        self._connection.priority_changed.connect(self._on_priority_changed)
        self._tel_port = tel_port
        self._cmd_port = cmd_port
        self._discovered_host: Optional[str] = None
        self._discovered_cmd_port: Optional[int] = None
        self._mode_panel = ModePanel(self._dispatcher)
        self._heater_panel = HeaterPanel(self._dispatcher)
        self._stepper_panel = StepperPanel(self._dispatcher)
        self._command_panel = CommandPanel(self._dispatcher)
        self._command_panel.debug_armed_changed.connect(self._heater_panel.set_armed)

        left_splitter = QSplitter(Qt.Orientation.Vertical)
        left_splitter.setChildrenCollapsible(True)
        for panel, stretch in [
            (self._connection,    0),
            (self._mode_panel,    1),
            (self._heater_panel,  3),
            (self._stepper_panel, 2),
            (self._command_panel, 1),
        ]:
            panel.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
            left_splitter.addWidget(panel)
            left_splitter.setStretchFactor(left_splitter.count() - 1, stretch)

        left_scroll = QScrollArea()
        left_scroll.setWidgetResizable(True)
        left_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        left_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        left_scroll.setWidget(left_splitter)

        left_dock = QDockWidget("Controls", self)
        left_dock.setWidget(left_scroll)
        left_dock.setAllowedAreas(Qt.DockWidgetArea.LeftDockWidgetArea | Qt.DockWidgetArea.RightDockWidgetArea)
        left_dock.setMinimumWidth(360)
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
        right_dock.setMinimumWidth(320)
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

        # ── discovery: beacon + passive listener start immediately ──
        self._beacon = GsBeacon(
            tel_port=tel_port, cmd_port=cmd_port,
            priority=self._connection.current_priority(),
            discovery_port=DISCOVERY_PORT_DEFAULT,
        )
        self._beacon.log_message.connect(self._log.append)
        self._listener = OnboardListener(discovery_port=DISCOVERY_PORT_DEFAULT)
        self._listener.log_message.connect(self._log.append)
        self._listener.onboard_discovered.connect(self._on_onboard_discovered)
        self._listener.peer_gs_seen.connect(self._on_peer_gs_seen)
        self._beacon.start()
        self._listener.start()

        # ── restore geometry ──
        # ── firewall / network-profile auto-configure (Windows only) ──
        if firewall_check:
            try:
                fw_result = firewall.check_and_prompt(self)
            except Exception as exc:  # never block GUI startup on this
                fw_result = "failed"
                self._log.append(f"[firewall] probe error: {exc}")
            if fw_result == "ok":
                self._log.append("[firewall] rules OK")
            elif fw_result == "deferred":
                self._log.append(
                    "[firewall] not configured — Pi may not reach GS; "
                    "see docs/firewall.md"
                )
                self.statusBar().showMessage(
                    "firewall not configured — Pi may not reach GS; "
                    "see docs/firewall.md",
                    10_000,
                )
            elif fw_result == "failed":
                self._log.append(
                    "[firewall] auto-configure failed — see docs/firewall.md "
                    "to run scripts/configure_firewall.ps1 manually"
                )
        else:
            self._log.append("[firewall] check skipped (--no-firewall-check)")

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
        self._receiver.status_changed.connect(self._on_receiver_status)
        self._receiver.start()

    def _on_receiver_status(self, state: str) -> None:
        self._connection.set_status(state)
        colors = {"listening": "#3498db", "connected": "#2ecc71",
                  "stale": "#f39c12", "searching": "#f39c12"}
        self._top.set_discovery(f"tel: {state}", colors.get(state, "#888"))

    def _on_priority_changed(self, p: int) -> None:
        if hasattr(self, "_beacon") and self._beacon is not None:
            self._beacon.set_priority(int(p))
            self._log.append(f"[discovery] beacon priority => {p}")

    def _on_onboard_discovered(self, host: str, cmd_port: int, tel_port: int,
                               session: str, hostname: str) -> None:
        self._connection.set_discovered(host, cmd_port, tel_port, session, hostname)
        # Only retarget dispatcher if operator left host blank OR a new host/port appeared.
        user_host = self._connection.onboard_host_value()
        retarget = (not user_host) and (
            host != self._discovered_host or cmd_port != self._discovered_cmd_port
        )
        if retarget:
            self._dispatcher.set_endpoint(host, cmd_port)
            self._discovered_host = host
            self._discovered_cmd_port = cmd_port
            self._log.append(f"[discovery] command target => {host}:{cmd_port}")
            self._top.set_discovery(f"cmd: {host}:{cmd_port}", "#2ecc71")
            try:
                Toast.anchor(self._connection, f"Found {hostname} @ {host}", ok=True)
            except RuntimeError:
                pass
        elif host != self._discovered_host or cmd_port != self._discovered_cmd_port:
            self._discovered_host = host
            self._discovered_cmd_port = cmd_port
            self._log.append(
                f"[discovery] onboard seen at {host}:{cmd_port} "
                f"(user override {user_host} in effect)"
            )

    def _on_peer_gs_seen(self, host: str, priority: int) -> None:
        self._log.append(f"[discovery] peer GS seen at {host} priority={priority}")

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
        if getattr(self, "_beacon", None) is not None:
            self._beacon.stop(); self._beacon.wait(2000)
        if getattr(self, "_listener", None) is not None:
            self._listener.stop(); self._listener.wait(2000)
        super().closeEvent(event)


def run_gui(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="COATHEAL Ground Station")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--tel-port", type=int, default=4000)
    parser.add_argument("--cmd-port", type=int, default=5000)
    parser.add_argument("--host", default="169.254.10.10")
    parser.add_argument("--log", type=Path, default=Path("logs/ground_telemetry.csv"))
    parser.add_argument("--no-firewall-check", action="store_true",
                        help="Skip the Windows firewall / network-profile auto-check at startup.")
    args = parser.parse_args(argv)

    app = QApplication.instance() or QApplication([])
    from .theme import apply_dark_palette
    apply_dark_palette(app)

    win = MainWindow(bind=args.bind, tel_port=args.tel_port,
                     cmd_port=args.cmd_port, cmd_host=args.host,
                     log_path=args.log,
                     firewall_check=not args.no_firewall_check)
    win.show()
    return app.exec()
