"""Main window — the fixed mission console (redesign spec §4).

Regions: menu · top strip · alarm strip · [left tabs | plots | right tabs]
· [console | events/pulls] · status bar. Every panel consumes one
`OnboardState` per frame (and every 500 ms for link age) and sends through
one `CommandDispatcher` whose responses fan out to the console, the
session's commands.csv and the panel that asked.
"""
from __future__ import annotations

import argparse
import dataclasses
import time
from pathlib import Path
from typing import Optional

import pyqtgraph as pg
from PyQt6.QtCore import QSettings, Qt, QTimer
from PyQt6.QtGui import QAction, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QApplication, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
    QSplitter, QStatusBar, QTabWidget, QTextEdit, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, PullEvent, TelemetryPacket
from ..telemetry_log import LogManager
from ..thermal_presets import PresetStore
from . import firewall
from .alarms import AlarmModel
from .dispatch import CommandDispatcher, TelemetryReceiver
from .discovery import (
    DISCOVERY_PORT_DEFAULT, CommandProbe, GsBeacon, OnboardListener, SentNonceRegistry,
)
from .panel_checkout import CheckoutPanel
from .panel_console import ConsolePanel
from .panel_events import EventsPanel, PullsPanel
from .panel_top import AlarmStrip, TopStrip
from .panel_values import ValuesPanel
from .panels_health import HealthPanel
from .plots import PlotArea
from .scale import UiScale
from .state import OnboardState, state_from_packet
from .tab_advanced import AdvancedTab
from .tab_motion import MotionTab
from .tab_system import SystemTab
from .tab_thermal import ThermalTab

STATE_TICK_MS = 500
DISK_TICK_MS = 10_000


class MainWindow(QMainWindow):
    def __init__(self, *, bind: str, tel_port: int, cmd_port: int, cmd_host: str,
                 log_path: Path, firewall_check: bool = True,
                 preset_store: Optional[PresetStore] = None):
        super().__init__()
        self.setWindowTitle("COATHEAL Ground Station")
        self.resize(1600, 900)
        self._settings = QSettings("COATHEAL", "GroundStation")
        app = QApplication.instance()
        self._scale = UiScale(app, self._settings) if app is not None else None

        # `log_path` is the log ROOT (`logs/`); a legacy file path such as
        # `logs/x.csv` resolves to its parent (spec §7).
        self._log_root = log_path if log_path.suffix == "" else log_path.parent
        self._logs = LogManager(self._log_root, gs_info={
            "component": "gui", "bind": bind, "tel_port": tel_port, "cmd_port": cmd_port,
        })
        self._bind = bind
        self._tel_port = tel_port
        self._cmd_port = cmd_port
        self._user_host = (cmd_host or "").strip()
        self._receiver: Optional[TelemetryReceiver] = None
        self._receiver_state = "idle"
        self._link_ok = False
        self._discovered_host: Optional[str] = None
        self._discovered_cmd_port: Optional[int] = None
        self._target_how = ""
        self._last_pkt: Optional[TelemetryPacket] = None
        self._last_rx_mono: Optional[float] = None
        self._frames = 0
        self._parse_errors = 0
        self._state = OnboardState()
        self._alarms = AlarmModel()
        self._beep = bool(self._settings.value("alarms/beep", False, type=bool))

        self._dispatcher = CommandDispatcher(cmd_host, cmd_port, log_manager=self._logs)
        self._dispatcher.response_received.connect(self._on_response)
        self._dispatcher.silence_changed.connect(self._on_silence_changed)
        self._presets = preset_store if preset_store is not None else PresetStore().load()

        pg.setConfigOption("background", "#0d0d0d")
        pg.setConfigOption("foreground", "#cccccc")

        # ── widgets ──
        self._top = TopStrip(self._dispatcher)
        self._alarm_strip = AlarmStrip()
        self._alarm_strip.ack_requested.connect(self._ack_alarm)
        self._alarm_strip.ack_all_requested.connect(self._ack_all_alarms)
        self._system = SystemTab(self._dispatcher)
        self._system.btn_restart_receiver.clicked.connect(
            lambda: self._on_start_telemetry(self._bind, self._tel_port, self._cmd_port, self._user_host))
        self._thermal = ThermalTab(self._dispatcher, self._presets)
        self._motion = MotionTab(self._dispatcher)
        self._advanced = AdvancedTab(self._dispatcher, self._presets, bind=bind, tel_port=tel_port,
                                     cmd_port=cmd_port, discovery_port=DISCOVERY_PORT_DEFAULT,
                                     host_override=self._user_host)
        self._advanced.gains_changed.connect(self._thermal.set_gains)
        self._advanced.priority_changed.connect(self._on_priority_changed)
        self._advanced.host_override_changed.connect(self._on_host_override)
        self._left_tabs = QTabWidget(); self._left_tabs.setObjectName("leftTabs")
        for widget, title in ((self._system, "System"), (self._thermal, "Thermal"),
                              (self._motion, "Motion"), (self._advanced, "Advanced")):
            self._left_tabs.addTab(widget, title)
        self._left_tabs.setMinimumWidth(410)

        self._plots = PlotArea()
        self._plots.setMinimumWidth(380)
        self._thermal.targets_changed.connect(lambda targets: self._plots.set_targets(targets))

        self._health = HealthPanel()
        self._checkout = CheckoutPanel(self._dispatcher)
        self._values = ValuesPanel()
        self._right_tabs = QTabWidget(); self._right_tabs.setObjectName("rightTabs")
        self._right_tabs.addTab(self._health, "Health")
        self._right_tabs.addTab(self._checkout, "Checkout")
        self._right_tabs.addTab(self._values, "Values")
        self._right_tabs.setMinimumWidth(300)

        self._console = ConsolePanel()
        self._console.send_requested.connect(lambda cmd: self._dispatcher.send(cmd, tag=self._console))
        self._events = EventsPanel()
        self._events.set_sink(self._logs.log_event)
        self._pulls = PullsPanel()
        self._bottom_tabs = QTabWidget(); self._bottom_tabs.setObjectName("bottomTabs")
        self._bottom_tabs.addTab(self._events, "Events")
        self._bottom_tabs.addTab(self._pulls, "Pulls")
        self._bottom = QSplitter(Qt.Orientation.Horizontal); self._bottom.setObjectName("bottomSplit")
        self._bottom.addWidget(self._console); self._bottom.addWidget(self._bottom_tabs)
        self._bottom.setStretchFactor(0, 3); self._bottom.setStretchFactor(1, 2)
        self._bottom.setMinimumHeight(150)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal); self._main_splitter.setObjectName("mainSplit")
        for widget, stretch in ((self._left_tabs, 3), (self._plots, 6), (self._right_tabs, 3)):
            self._main_splitter.addWidget(widget)
            self._main_splitter.setStretchFactor(self._main_splitter.count() - 1, stretch)
        self._main_splitter.setCollapsible(1, False)
        self._main_splitter.setSizes([440, 840, 320])
        self._body_splitter = QSplitter(Qt.Orientation.Vertical); self._body_splitter.setObjectName("bodySplit")
        self._body_splitter.addWidget(self._main_splitter); self._body_splitter.addWidget(self._bottom)
        self._body_splitter.setStretchFactor(0, 4); self._body_splitter.setStretchFactor(1, 1)
        self._body_splitter.setCollapsible(0, False)
        self._body_splitter.setSizes([640, 220])

        central = QWidget()
        v = QVBoxLayout(central); v.setContentsMargins(0, 0, 0, 0); v.setSpacing(0)
        v.addWidget(self._top); v.addWidget(self._alarm_strip); v.addWidget(self._body_splitter, 1)
        self.setCentralWidget(central)
        self.setStatusBar(QStatusBar())
        self._status_disk = ""
        self._update_status_bar()

        self._build_menus()
        self._build_shortcuts()

        # ── discovery: beacon + passive listener + command probe ──
        self._sent_nonces = SentNonceRegistry()
        self._beacon = GsBeacon(tel_port=tel_port, cmd_port=cmd_port, priority=self._advanced.priority.value(),
                                discovery_port=DISCOVERY_PORT_DEFAULT, sent_nonces=self._sent_nonces)
        self._beacon.log_message.connect(self._events.append)
        self._listener = OnboardListener(discovery_port=DISCOVERY_PORT_DEFAULT, sent_nonces=self._sent_nonces)
        self._listener.log_message.connect(self._events.append)
        self._listener.onboard_discovered.connect(self._on_onboard_discovered)
        self._listener.peer_gs_seen.connect(self._on_peer_gs_seen)
        self._probe = CommandProbe([cmd_host], cmd_port=cmd_port, include_static=not bool(cmd_host))
        self._probe.log_message.connect(self._events.append)
        self._probe.onboard_reachable.connect(self._on_onboard_reachable)
        self._beacon.start(); self._listener.start(); self._probe.start()

        self._on_start_telemetry(bind, tel_port, cmd_port, cmd_host)
        self._refresh_link_info()

        if firewall_check:
            try:
                fw_result = firewall.check_and_prompt(self)
            except Exception as exc:  # never block GUI startup on this
                fw_result = "failed"
                self._events.append(f"[firewall] probe error: {exc}")
            if fw_result == "ok":
                self._events.append("[firewall] rules OK")
            elif fw_result == "deferred":
                self._events.append("[firewall] not configured — Pi may not reach GS; see docs/firewall.md")
            elif fw_result == "failed":
                self._events.append("[firewall] auto-configure failed — see docs/firewall.md")
        else:
            self._events.append("[firewall] check skipped (--no-firewall-check)")

        self._state_timer = QTimer(self); self._state_timer.timeout.connect(self._tick); self._state_timer.start(STATE_TICK_MS)
        self._disk_timer = QTimer(self); self._disk_timer.timeout.connect(self._refresh_disk); self._disk_timer.start(DISK_TICK_MS)

        geo = self._settings.value("window/geometry")
        if geo:
            self.restoreGeometry(geo)
        state = self._settings.value("window/state")
        if state:
            self.restoreState(state)
        for key, splitter in (("splitter/main", self._main_splitter), ("splitter/body", self._body_splitter),
                              ("splitter/bottom", self._bottom)):
            saved = self._settings.value(key)
            if saved:
                splitter.restoreState(saved)
        self._apply_state()

    # ── telemetry plumbing ──
    def _on_start_telemetry(self, bind: str, tel_port: int, cmd_port: int, cmd_host: str) -> None:
        self._dispatcher.set_endpoint(cmd_host, cmd_port)
        if self._receiver is not None and self._receiver.isRunning():
            self._events.append("[telemetry] receiver already running — ignoring")
            return
        self._receiver = TelemetryReceiver(bind, tel_port, self._logs)
        self._receiver.packet_received.connect(self._on_packet)
        self._receiver.pull_event.connect(self._on_pull_event)
        self._receiver.log_message.connect(self._events.append)
        self._receiver.connection_changed.connect(self._on_connection_changed)
        self._receiver.status_changed.connect(self._on_receiver_status)
        self._receiver.session_opened.connect(self._on_session_opened)
        self._receiver.start()

    def _on_receiver_status(self, state: str) -> None:
        if self.sender() is not self._receiver:
            # A stale signal from a superseded receiver (a retry built
            # receiver #2 while a queued "failed" from dead #1 was still in
            # flight) must not null out the live receiver.
            return
        self._receiver_state = state
        if state == "failed":
            self._receiver = None
            self._link_ok = False
        self._top.set_receiver_state(state)
        self._refresh_link_info()
        self._apply_state()

    def _on_connection_changed(self, connected: bool, addr: str) -> None:
        if self.sender() is not self._receiver:
            return
        self._link_ok = connected
        if connected:
            host = addr.rsplit(":", 1)[0].strip()
            if host and not self._user_host and host != self._discovered_host:
                self._set_target(host, self._cmd_port, "telemetry peer")
        self.statusBar().showMessage(f"Connected: {addr}" if connected else "Waiting for onboard…", 5000)
        self._refresh_link_info()

    def _on_session_opened(self, session_id: str, directory: str) -> None:
        self._update_status_bar()
        self._events.append(f"[log] session {session_id} → {directory}")

    def _on_packet(self, pkt: TelemetryPacket) -> None:
        now_mono = time.monotonic()
        rx_time = time.time()
        self._last_pkt = pkt
        self._last_rx_mono = now_mono
        self._frames += 1
        self._top.on_packet_received(pkt.session_id, now_mono)
        self._top.set_health(pkt)
        self._plots.on_packet(pkt, rx_time)
        self._health.on_packet(pkt)
        self._apply_state()
        self._values.on_packet(pkt, self._state)
        if self._frames % 20 == 0:
            self._update_status_bar()

    def _on_pull_event(self, ev: PullEvent) -> None:
        self._pulls.on_pull_event(ev)
        self._motion.on_pull_event(ev)
        self._plots.on_pull_event(ev, time.time())

    def _on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        body = resp.body if resp.ok else (resp.error or resp.raw)
        self._events.append(f"[{'ACK' if resp.ok else 'NACK'}] {cmd}  ({ms:.0f} ms)  {body}",
                            "INFO" if resp.ok else "WARN")
        self._console.on_response(cmd, resp, ms, tag)
        for panel in (self._system, self._thermal, self._motion, self._advanced, self._checkout):
            panel.on_response(cmd, resp, ms, tag)

    def _on_silence_changed(self, active: bool) -> None:
        for worker in (self._beacon, self._probe):
            worker.set_quiet(active)
        self._events.append("[radio] silence ACTIVE — beacons/probes paused, only RADIO_RESUME / STATUS / PING are sent"
                            if active else "[radio] silence lifted — discovery resumed", "WARN" if active else "INFO")
        self._console.set_note("radio silence: only RADIO_RESUME, STATUS and PING are sent" if active else "")
        self._apply_state()

    # ── state fan-out ──
    def _link_age(self) -> Optional[float]:
        return None if self._last_rx_mono is None else time.monotonic() - self._last_rx_mono

    def _apply_state(self) -> None:
        silence = self._dispatcher.silence
        age = self._link_age()
        if self._last_pkt is not None:
            state = state_from_packet(self._last_pkt, silence=silence, link_age_s=age)
        else:
            state = dataclasses.replace(OnboardState(), silence=silence, link_age_s=age)
        self._state = state
        alarms = self._alarms.update(state)
        if self._alarms.new_keys:
            for key in self._alarms.new_keys:
                self._events.append(f"[alarm] {next((a.text for a in alarms if a.key == key), key)}", "WARN")
            if self._beep:
                QApplication.beep()
        self._alarm_strip.set_alarms(alarms)
        self._top.set_state(state)
        self._system.update_state(state)
        self._thermal.update_state(state)
        self._motion.update_state(state)
        self._advanced.update_state(state)
        self._checkout.update_state(state, link_ok=self._link_ok, unacked_alarms=self._alarms.unacked_count)

    def _tick(self) -> None:
        # Link age changes between frames; alarms and gating must follow it.
        self._apply_state()
        self._refresh_link_info()

    def _ack_alarm(self, key: str) -> None:
        self._alarms.acknowledge(key)
        self._events.append(f"[alarm] acknowledged {key}")
        self._alarm_strip.set_alarms(self._alarms.active)

    def _ack_all_alarms(self) -> None:
        self._alarms.acknowledge_all()
        self._events.append("[alarm] all acknowledged")
        self._alarm_strip.set_alarms(self._alarms.active)

    # ── discovery ──
    def _set_target(self, host: str, cmd_port: int, how: str) -> None:
        self._dispatcher.set_endpoint(host, cmd_port)
        self._discovered_host = host
        self._discovered_cmd_port = cmd_port
        self._target_how = how
        self._events.append(f"[discovery] command target => {host}:{cmd_port} ({how})")
        self._refresh_link_info()

    def _refresh_link_info(self) -> None:
        target = f"{self._dispatcher.host}:{self._dispatcher.port}"
        how = "manual" if self._user_host else (self._target_how or "static")
        self._top.set_target(f"{target} ({how})")
        rate = None
        if self._last_rx_mono is not None:
            rate = len(self._top._rx_times) / TopStrip.RATE_WINDOW_S
        self._system.set_link_info(target=target, target_how=how, receiver=self._receiver_state,
                                   rate_hz=rate, age_s=self._link_age())

    def _on_priority_changed(self, p: int) -> None:
        self._beacon.set_priority(int(p))
        self._events.append(f"[discovery] beacon priority => {p}")

    def _on_host_override(self, host: str) -> None:
        self._user_host = host
        if host:
            self._dispatcher.set_endpoint(host, self._cmd_port)
            self._probe.set_candidates([host])
            self._events.append(f"[discovery] manual command target => {host}:{self._cmd_port}")
        else:
            self._probe.set_candidates([self._discovered_host or ""])
            self._events.append("[discovery] manual override cleared — back to discovery")
        self._refresh_link_info()

    def _on_onboard_discovered(self, host: str, cmd_port: int, tel_port: int, session: str, hostname: str) -> None:
        self._probe.set_candidates([host, self._user_host])
        if not self._user_host and (host != self._discovered_host or cmd_port != self._discovered_cmd_port):
            self._set_target(host, cmd_port, f"discovery {hostname}")
        elif host != self._discovered_host or cmd_port != self._discovered_cmd_port:
            self._discovered_host = host
            self._discovered_cmd_port = cmd_port
            self._events.append(f"[discovery] onboard seen at {host}:{cmd_port} (manual override {self._user_host} in effect)")

    def _on_peer_gs_seen(self, host: str, priority: int) -> None:
        self._events.append(f"[discovery] peer GS seen at {host} priority={priority}", "WARN")

    def _on_onboard_reachable(self, host: str, cmd_port: int) -> None:
        if self._user_host and self._user_host != host:
            return
        if host == self._discovered_host and cmd_port == self._discovered_cmd_port:
            return
        self._set_target(host, cmd_port, "probe")

    # ── status bar ──
    def _update_status_bar(self) -> None:
        directory = self._logs.current_dir
        where = f"sessions/{directory.name}" if directory else "no session yet"
        scale = f" · UI {self._scale.percent} %" if self._scale else ""
        self.statusBar().showMessage(
            f"{self._log_root} · {where} · {self._frames} frames · {self._parse_errors} parse errors"
            f"{self._status_disk}{scale} · Esc = STOP MOTORS · F1 shortcuts")
        self.statusBar().setToolTip(str(directory) if directory else str(self._log_root))

    def _refresh_disk(self) -> None:
        directory = self._logs.current_dir
        if directory is None:
            return
        try:
            size = sum(p.stat().st_size for p in directory.iterdir() if p.is_file())
            self._status_disk = f" · {size / 1e6:.1f} MB on disk"
        except OSError:
            self._status_disk = ""
        self._update_status_bar()

    # ── menus / shortcuts ──
    def _build_menus(self) -> None:
        file_menu = self.menuBar().addMenu("&File")
        act = QAction("Clear plots", self); act.triggered.connect(self._plots.clear); file_menu.addAction(act)
        act = QAction("Export current plot…", self); act.triggered.connect(self._plots.export_dialog); file_menu.addAction(act)
        act = QAction("Quit", self); act.setShortcut("Ctrl+Q"); act.triggered.connect(self.close); file_menu.addAction(act)

        view_menu = self.menuBar().addMenu("&View")
        self._act_right = QAction("Right column", self, checkable=True, checked=True)
        self._act_right.toggled.connect(self._right_tabs.setVisible)
        self._act_bottom = QAction("Console and events", self, checkable=True, checked=True)
        self._act_bottom.toggled.connect(self._bottom.setVisible)
        self._act_beep = QAction("Audible alarms", self, checkable=True, checked=self._beep)
        self._act_beep.toggled.connect(self._set_beep)
        for act in (self._act_right, self._act_bottom, self._act_beep):
            view_menu.addAction(act)
        view_menu.addSeparator()
        for label, keys, slot in (("Larger UI", "Ctrl+=", self._zoom_in), ("Smaller UI", "Ctrl+-", self._zoom_out),
                                  ("Reset UI size", "Ctrl+0", self._zoom_reset)):
            act = QAction(label, self); act.setShortcut(keys); act.triggered.connect(slot); view_menu.addAction(act)

        help_menu = self.menuBar().addMenu("&Help")
        act = QAction("Keyboard shortcuts", self); act.setShortcut("F1"); act.triggered.connect(self._show_cheatsheet)
        help_menu.addAction(act)

    def _set_beep(self, enabled: bool) -> None:
        self._beep = bool(enabled)
        self._settings.setValue("alarms/beep", self._beep)

    def _zoom_in(self) -> None:
        if self._scale: self._scale.zoom_in(); self._update_status_bar()

    def _zoom_out(self) -> None:
        if self._scale: self._scale.zoom_out(); self._update_status_bar()

    def _zoom_reset(self) -> None:
        if self._scale: self._scale.reset(); self._update_status_bar()

    SHORTCUTS = (
        ("Esc", "STEPPER_STOP 0 + STEPPER_STOP 1 (panic, no confirm)"),
        ("Ctrl+Shift+H", "HEATERS_OFF (panic, no confirm)"),
        ("Ctrl+L", "focus the console entry"),
        ("Ctrl+1 … Ctrl+4", "System / Thermal / Motion / Advanced"),
        ("Alt+1 … Alt+5", "plot tabs"),
        ("P", "pause / resume plots (ignored while typing)"),
        ("F5", "send STATUS"),
        ("Ctrl+= / Ctrl+- / Ctrl+0", "UI size"),
        ("F1", "this list"),
    )

    def _show_cheatsheet(self) -> None:
        lines = [f"{keys:<24} {what}" for keys, what in self.SHORTCUTS]
        lines.append("")
        lines.append("Glyphs: ✔ ACK  ✖ NACK  ⚠ alarm  ● indicator (green OK, amber attention, red fault, grey unknown)")
        lines.append("While a confirmation dialog is open every shortcut is blocked; Esc closes the dialog first.")
        QMessageBox.information(self, "Shortcuts", "\n".join(lines))

    def _build_shortcuts(self) -> None:
        def sc(keys: str, slot):
            s = QShortcut(QKeySequence(keys), self)
            s.setContext(Qt.ShortcutContext.ApplicationShortcut)
            s.activated.connect(slot)
            return s
        sc("Esc", self._motion.stop_all)
        sc("Ctrl+Shift+H", lambda: self._dispatcher.send("HEATERS_OFF", tag=self._top))
        sc("Ctrl+L", self._console.focus_entry)
        for i in range(4):
            sc(f"Ctrl+{i + 1}", lambda idx=i: self._left_tabs.setCurrentIndex(idx))
        for i in range(5):
            sc(f"Alt+{i + 1}", lambda idx=i: self._plots.tabs.setCurrentIndex(idx))
        sc("P", self._toggle_pause)
        sc("F5", lambda: self._dispatcher.send("STATUS", tag=self._system))

    def _toggle_pause(self) -> None:
        focus = QApplication.focusWidget()
        if isinstance(focus, (QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox)):
            return
        paused = self._plots.toggle_paused()
        self.statusBar().showMessage("Plots paused (P to resume)" if paused else "Plots live", 2000)

    # ── lifecycle ──
    def closeEvent(self, event) -> None:  # noqa: N802
        self._settings.setValue("window/geometry", self.saveGeometry())
        self._settings.setValue("window/state", self.saveState())
        self._settings.setValue("splitter/main", self._main_splitter.saveState())
        self._settings.setValue("splitter/body", self._body_splitter.saveState())
        self._settings.setValue("splitter/bottom", self._bottom.saveState())
        if self._receiver is not None:
            self._receiver.stop(); self._receiver.wait(2000)
        for worker in (self._beacon, self._listener, self._probe):
            worker.stop(); worker.wait(2000)
        self._logs.close()
        super().closeEvent(event)


def run_gui(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="COATHEAL Ground Station")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--tel-port", type=int, default=4000)
    parser.add_argument("--cmd-port", type=int, default=5000)
    parser.add_argument("--host", default="")
    parser.add_argument("--log", type=Path, default=Path("logs"),
                        help="Log root; each onboard session gets its own directory under <root>/sessions/.")
    parser.add_argument("--no-firewall-check", action="store_true",
                        help="Skip the Windows firewall / network-profile auto-check at startup.")
    args = parser.parse_args(argv)

    app = QApplication.instance() or QApplication([])
    from .theme import apply_dark_palette
    apply_dark_palette(app)

    win = MainWindow(bind=args.bind, tel_port=args.tel_port, cmd_port=args.cmd_port,
                     cmd_host=args.host, log_path=args.log,
                     firewall_check=not args.no_firewall_check)
    win.show()
    return app.exec()
