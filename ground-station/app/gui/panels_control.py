"""Control panels that sit in the left dock.

Sections:
  * ConnectionPanel — host/port + Start button + status line.
  * ModePanel      — big state tile + ARM/DISARM/SAFE/RADIO rows.
  * HeaterPanel    — 5x2 grid of HeaterCell with global presets.
  * StepperPanel   — motor-selectable jog, zeroing, and sequence controls.
  * CommandPanel   — diagnostics (PING/STATUS/TICK_HZ) + arbitrary command
                     entry (bench-only commands like ARM_DEBUG go through
                     the free-command box).
  * EmergencyBar   — always-visible red bar of panic actions.

Every button uses CommandDispatcher.send() with a tag so toasts anchor
correctly.
"""
from __future__ import annotations

import math
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QComboBox, QDoubleSpinBox, QFormLayout, QFrame, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QProgressBar, QPushButton,
    QSizePolicy, QSlider, QSpinBox, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget,
)

from ..protocol import (
    CommandResponse, TelemetryPacket,
    validate_duty, validate_heater_index, validate_microstep,
    validate_pid_gains, validate_temperature_target,
    validate_revolutions, validate_speed_hz, validate_stepper_move,
    validate_tick_hz,
)
from ..thermal_profiles import build_profile, load_profiles, save_profiles
from .dispatch import CommandDispatcher
from .theme import HEATER_COLORS, HEATER_LABELS, mode_color, phase_color
from .widgets import StatusDot, Toast, confirm


# Semantic button classes. Pick by what the action *does*, not by taste:
#   danger  — anything that stops/kills motion, power, or the run (STOP,
#             HEATERS_OFF, DISABLE, ENTER_SAFE, RADIO_SILENCE, ...)
#   success — arm/start/run/resume actions that bring something to life
#   primary — the default action color for ordinary command sends
#   neutral — read-only queries and low-stakes config writes/clears
_BUTTON_CLASSES = {
    "primary": "#2980b9",
    "success": "#27ae60",
    "danger":  "#c0392b",
    "neutral": "#7f8c8d",
}


def _style_button(btn: QPushButton, *, cls: Optional[str] = None, bg: Optional[str] = None,
                  fg: str = "white", bold: bool = False, min_height: int = 26) -> None:
    """Apply consistent QPushButton styling.

    Prefer `cls` (one of "primary"/"success"/"danger"/"neutral") for
    anything that dispatches a command -- it keeps color meaning
    consistent across panels. `bg` is still available for literal colors
    that encode data rather than a safety semantic (e.g. heater
    duty-percentage presets, whose color ramps with magnitude, not
    danger).
    """
    if bg is None:
        bg = _BUTTON_CLASSES[cls or "primary"]
    weight = "bold" if bold else "normal"
    btn.setMinimumHeight(min_height)
    btn.setStyleSheet(
        f"QPushButton {{ background: {bg}; color: {fg}; font-weight: {weight}; "
        f"border: 1px solid #222; border-radius: 3px; padding: 3px 8px; }}"
        f"QPushButton:hover {{ background: #3a3a3a; }}"
        f"QPushButton:disabled {{ background: #1a1a1a; color: #555; }}"
    )


# ── Connection ───────────────────────────────────────────────────────────────
class ConnectionPanel(QGroupBox):
    start_requested = pyqtSignal(str, int, int, str)  # bind, tel_port, cmd_port, cmd_host
    priority_changed = pyqtSignal(int)

    def __init__(self, default_bind: str, default_tel: int, default_cmd_port: int,
                 default_cmd_host: str, parent=None, *, default_priority: int = 100):
        super().__init__("Connection", parent)
        form = QFormLayout(self)
        form.setContentsMargins(6, 6, 6, 6)

        self._bind = QLineEdit(default_bind)
        self._tel_port = QSpinBox(); self._tel_port.setRange(1, 65535); self._tel_port.setValue(default_tel)
        self._cmd_host = QLineEdit(default_cmd_host if default_cmd_host else "")
        self._cmd_host.setPlaceholderText("auto / 169.254.10.10")
        self._cmd_port = QSpinBox(); self._cmd_port.setRange(1, 65535); self._cmd_port.setValue(default_cmd_port)
        self._priority = QSpinBox(); self._priority.setRange(0, 999); self._priority.setValue(default_priority)
        self._priority.setToolTip("GS beacon priority — higher wins. Backup GS should be lower (e.g. 50).")
        self._priority.valueChanged.connect(self.priority_changed.emit)
        form.addRow("Bind IP:",    self._bind)
        form.addRow("Tel port:",   self._tel_port)
        form.addRow("Onboard IP:", self._cmd_host)
        form.addRow("Cmd port:",   self._cmd_port)
        form.addRow("Priority:",   self._priority)

        self._start_btn = QPushButton("▶ Start Telemetry")
        _style_button(self._start_btn, cls="primary", bold=True, min_height=30)
        self._start_btn.setToolTip(
            "Starts the local telemetry receiver — does not send a wire command."
        )
        self._start_btn.clicked.connect(self._emit_start)
        form.addRow(self._start_btn)

        self._status = QLabel("● idle")
        self._status.setStyleSheet("color: #888; font-size: 11pt;")
        form.addRow(self._status)

        self._discovered = QLabel("Discovered: —")
        self._discovered.setStyleSheet("color: #888; font-family: monospace; font-size: 10pt;")
        self._discovered.setWordWrap(True)
        form.addRow(self._discovered)

    def _emit_start(self) -> None:
        self.start_requested.emit(self._bind.text().strip(), self._tel_port.value(),
                                  self._cmd_port.value(), self._cmd_host.text().strip())
        # No optimistic flip here. `set_receiver_running(True)` is only
        # ever called by MainWindow, from a receiver-proven
        # "listening"/"connected" signal -- the same rule the boot
        # auto-start path follows. That keeps exactly one writer of the
        # "running" claim across the whole surface: the receiver itself.

    def set_receiver_running(self, running: bool) -> None:
        """Reflect receiver start/stop in the button — used both by the
        manual click path and by MainWindow's telemetry auto-start so the
        button never lies about whether a receiver is actually up."""
        self._start_btn.setEnabled(not running)
        self._start_btn.setText("■ Receiver running" if running else "▶ Start Telemetry")

    def set_connected(self, connected: bool, addr: str) -> None:
        if connected:
            self._status.setText(f"● connected {addr}")
            self._status.setStyleSheet("color: #2ecc71; font-size: 11pt;")
        else:
            self._status.setText("● waiting for onboard…")
            self._status.setStyleSheet("color: #f39c12; font-size: 11pt;")

    def set_status(self, state: str) -> None:
        colors = {
            "listening": ("#3498db", "● listening"),
            "connected": ("#2ecc71", "● connected"),
            "stale":     ("#f39c12", "● stale (no data)"),
            "searching": ("#f39c12", "● searching for onboard…"),
            "failed":    ("#e74c3c", "● failed to start (port in use?)"),
        }
        color, text = colors.get(state, ("#888", f"● {state}"))
        self._status.setText(text)
        self._status.setStyleSheet(f"color: {color}; font-size: 11pt;")

    def set_discovered(self, host: str, cmd_port: int, tel_port: int,
                       session: str, hostname: str) -> None:
        self._discovered.setText(
            f"Discovered: {hostname}@{host}  cmd:{cmd_port} tel:{tel_port}  sess:{session[:8]}"
        )
        self._discovered.setStyleSheet(
            "color: #2ecc71; font-family: monospace; font-size: 10pt;"
        )

    def current_priority(self) -> int:
        return int(self._priority.value())

    def onboard_host_value(self) -> str:
        return self._cmd_host.text().strip()


# ── Mode / Safety ─────────────────────────────────────────────────────────────
class ModePanel(QGroupBox):
    PHASES = ["BOOT", "ASCENT", "PRE_FLOAT", "FLOAT", "DESCENT", "LANDED", "STOPPED"]

    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Mode / Phase", parent)
        self._disp = dispatcher
        self._radio_silent = False

        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        # Big state tile
        self._tile = QLabel("STANDBY")
        self._tile.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._tile.setMinimumHeight(48)
        self._tile.setStyleSheet(
            "background: #7f8c8d; color: white; font-size: 16pt; font-weight: bold; "
            "border-radius: 4px;"
        )
        outer.addWidget(self._tile)

        self._phase_label = QLabel("Phase: —")
        self._phase_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._phase_label.setStyleSheet("font-size: 11pt; color: #bbb;")
        outer.addWidget(self._phase_label)

        phase_row = QHBoxLayout(); outer.addLayout(phase_row)
        self._phase_select = QComboBox()
        self._phase_select.addItems(self.PHASES)
        phase_row.addWidget(self._phase_select, 1)
        self._btn_set_phase = self._mk_cmd(
            phase_row, "SET PHASE", "SET_PHASE <phase>", cls="primary",
            confirm_title="Set mission phase?")

        # ARM/DISARM/FORCE START/FORCE STOP: 2x2 grid rather than a 4-across
        # row, which used to wrap badly (and truncate labels) at narrow
        # dock widths.
        # Columns deliberately group by class: left = start-like (success),
        # right = stop-like (danger) — DISARM and FORCE STOP both halt
        # outputs/motion per docs/protocol.md, same tier as an emergency stop.
        row1 = QGridLayout(); row1.setSpacing(4); outer.addLayout(row1)
        self._arm_grid = row1  # test-only accessor for the 2x2 arrangement
        self._btn_arm   = self._mk_cmd(row1, "ARM",         "ARM",         cls="success", confirm_title="Arm experiment?", pos=(0, 0))
        self._btn_dis   = self._mk_cmd(row1, "DISARM",      "DISARM",      cls="danger",  pos=(0, 1))
        self._btn_start = self._mk_cmd(row1, "FORCE START", "FORCE_START", cls="success", pos=(1, 0))
        self._btn_stop  = self._mk_cmd(row1, "FORCE STOP",  "FORCE_STOP",  cls="danger",  confirm_title="Force stop?", pos=(1, 1))
        row1.setColumnStretch(0, 1); row1.setColumnStretch(1, 1)

        row2 = QHBoxLayout(); outer.addLayout(row2)
        self._btn_safe_in  = self._mk_cmd(row2, "ENTER SAFE",      "ENTER_SAFE",      cls="danger",  confirm_title="Enter SAFE mode?")
        self._btn_safe_out = self._mk_cmd(row2, "EXIT SAFE",       "EXIT_SAFE",       cls="success")

        row3 = QHBoxLayout(); outer.addLayout(row3)
        self._btn_radio_off = self._mk_cmd(row3, "RADIO SILENCE", "RADIO_SILENCE", cls="danger",  confirm_title="Silence downlink?")
        self._btn_radio_on  = self._mk_cmd(row3, "RADIO RESUME",  "RADIO_RESUME",  cls="success")
        self._btn_radio_on.setEnabled(False)

    def _mk_cmd(self, layout, label: str, wire_cmd: str, *, cls: str = "primary",
                confirm_title: Optional[str] = None,
                pos: Optional[tuple[int, int]] = None) -> QPushButton:
        btn = QPushButton(label)
        btn.setToolTip(f"Sends: {wire_cmd}")
        _style_button(btn, cls=cls, bold=True)
        btn.clicked.connect(lambda: self._fire(btn, wire_cmd, confirm_title))
        if pos is not None:
            layout.addWidget(btn, *pos)
        else:
            layout.addWidget(btn)
        return btn

    def _fire(self, btn: QPushButton, wire_cmd: str, confirm_title: Optional[str]) -> None:
        if wire_cmd.startswith("SET_PHASE "):
            # Resolve before the confirm prompt so the dialog names the
            # phase actually selected, not the tooltip's "<phase>" placeholder.
            wire_cmd = f"SET_PHASE {self._phase_select.currentText()}"
        if confirm_title is not None and not confirm(self, confirm_title, f"Send {wire_cmd}?"):
            return
        if wire_cmd == "RADIO_SILENCE":
            self._radio_silent = True
            self._btn_radio_on.setEnabled(True)
        elif wire_cmd == "RADIO_RESUME":
            self._radio_silent = False
            self._btn_radio_on.setEnabled(False)
        self._disp.send(wire_cmd, tag=btn)

    def on_response(self, cmd: str, resp: CommandResponse, tag) -> None:
        if isinstance(tag, QPushButton) and tag.parent() is self:
            Toast.anchor(self, resp.body if resp.ok else resp.error or "fail",
                         ok=resp.ok)

    def update_from_packet(self, pkt: TelemetryPacket) -> None:
        mode = (pkt.mode or "").upper() or "—"
        self._tile.setText(mode)
        self._tile.setStyleSheet(
            f"background: {mode_color(mode)}; color: white; "
            "font-size: 16pt; font-weight: bold; border-radius: 4px;"
        )
        self._phase_label.setText(f"Phase: {pkt.phase}")
        self._phase_label.setStyleSheet(f"font-size: 11pt; color: {phase_color(pkt.phase)};")
        if pkt.phase in self.PHASES:
            self._phase_select.setCurrentText(pkt.phase)


# ── Heater grid ───────────────────────────────────────────────────────────────
class HeaterCell(QFrame):
    set_requested = pyqtSignal(int, float)
    off_requested = pyqtSignal(int)
    target_requested = pyqtSignal(int, float)
    clear_target_requested = pyqtSignal(int)

    def __init__(self, idx: int, parent=None):
        super().__init__(parent)
        self.idx = idx
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setStyleSheet("QFrame { background: #1b1b1b; border: 1px solid #2a2a2a; border-radius: 3px; }")
        lay = QVBoxLayout(self); lay.setContentsMargins(4, 4, 4, 4); lay.setSpacing(2)

        header = QHBoxLayout()
        label_text = HEATER_LABELS[idx]
        self._label = QLabel(label_text)
        color = HEATER_COLORS[idx % len(HEATER_COLORS)]
        self._label.setStyleSheet(f"font-weight: bold; color: {color};")
        header.addWidget(self._label); header.addStretch()
        self._dot = StatusDot(8); self._dot.set_color("#333"); header.addWidget(self._dot)
        lay.addLayout(header)

        self._bar = QProgressBar(); self._bar.setRange(0, 1000); self._bar.setTextVisible(False)
        self._bar.setFixedHeight(10)
        self._style_bar("#555")
        lay.addWidget(self._bar)

        self._readout = QLabel("0.0%  |  —°C")
        self._readout.setStyleSheet("font-family: monospace; font-size: 10pt; color: #bbb;")
        lay.addWidget(self._readout)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, 100); self._slider.setSingleStep(5); self._slider.setPageStep(10)
        self._slider.setTickPosition(QSlider.TickPosition.NoTicks)
        self._slider.valueChanged.connect(self._on_slider_changed)
        lay.addWidget(self._slider)

        btns = QHBoxLayout()
        self._slider_val = QLabel("0%"); self._slider_val.setFixedWidth(34)
        self._slider_val.setStyleSheet("font-family: monospace; font-size: 10pt;")
        btns.addWidget(self._slider_val)
        set_btn = QPushButton("Set"); _style_button(set_btn, cls="primary", min_height=22)
        set_btn.setToolTip(f"Sends: SET_HEATER_DUTY {idx} <duty>")
        off_btn = QPushButton("Off"); _style_button(off_btn, cls="danger", min_height=22)
        off_btn.setToolTip(f"Sends: SET_HEATER_DUTY {idx} 0.0")
        set_btn.clicked.connect(lambda: self.set_requested.emit(self.idx, self._slider.value() / 100.0))
        off_btn.clicked.connect(lambda: self.off_requested.emit(self.idx))
        btns.addWidget(set_btn); btns.addWidget(off_btn)
        lay.addLayout(btns)

        target_row = QHBoxLayout()
        self._target = QDoubleSpinBox()
        self._target.setRange(0.0, 80.0)
        self._target.setDecimals(1)
        self._target.setValue(20.0)
        self._target.setSuffix(" C")
        target_btn = QPushButton("Target")
        _style_button(target_btn, cls="success", min_height=22)
        target_btn.setToolTip(f"Sends: SET_TEMP_TARGET {idx} <target_c>")
        clear_target = QPushButton("Clear")
        _style_button(clear_target, cls="neutral", min_height=22)
        clear_target.setToolTip(f"Sends: CLEAR_TEMP_TARGET {idx}")
        target_btn.clicked.connect(
            lambda: self.target_requested.emit(self.idx, self._target.value())
        )
        clear_target.clicked.connect(
            lambda: self.clear_target_requested.emit(self.idx)
        )
        target_row.addWidget(self._target, 1)
        target_row.addWidget(target_btn)
        target_row.addWidget(clear_target)
        lay.addLayout(target_row)

    def _style_bar(self, color: str) -> None:
        self._bar.setStyleSheet(
            "QProgressBar { border: 1px solid #333; border-radius: 2px; background: #0e0e0e; }"
            f"QProgressBar::chunk {{ background-color: {color}; border-radius: 1px; }}"
        )

    def _on_slider_changed(self, v: int) -> None:
        self._slider_val.setText(f"{v}%")

    def target_value(self) -> float:
        return float(self._target.value())

    def set_target_value(self, value: float) -> None:
        self._target.setValue(float(value))

    def update_live(self, duty: float, temp_c: Optional[float]) -> None:
        pct = max(0.0, min(1.0, float(duty))) * 100.0
        self._bar.setValue(int(pct * 10))
        color = "#2ecc71" if pct < 60 else "#f1c40f" if pct < 85 else "#e74c3c"
        self._style_bar(color)
        t_str = "—" if temp_c is None else f"{temp_c:5.1f}°C"
        self._readout.setText(f"{pct:5.1f}%  |  {t_str}")
        self._dot.set_color("#e74c3c" if pct > 0.5 else "#333")


class HeaterPanel(QGroupBox):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Heaters", parent)
        self._disp = dispatcher
        self._profile_path = Path("profiles/thermal_profiles.json")
        self._profiles = load_profiles(self._profile_path)

        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        self._banner = QLabel("Manual thermal controls")
        self._banner.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._banner.setStyleSheet(
            "background: #1a2d3a; color: #8fd3ff; padding: 4px; border-radius: 3px; "
            "font-weight: bold; font-size: 10pt;"
        )
        outer.addWidget(self._banner)

        # Final BOM: 6 heater cells (H0..H5); no box heater on the flight
        # hardware. Laid out 3x2 (two columns).
        grid = QGridLayout(); grid.setSpacing(4)
        self._cells: list[HeaterCell] = []
        num_cells = len(HEATER_LABELS)
        for i in range(num_cells):
            cell = HeaterCell(i)
            cell.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
            cell.set_requested.connect(self._set_one)
            cell.off_requested.connect(lambda idx: self._set_one(idx, 0.0))
            cell.target_requested.connect(self._set_target)
            cell.clear_target_requested.connect(self._clear_target)
            grid.addWidget(cell, i // 2, i % 2)
            self._cells.append(cell)
        grid.setColumnStretch(0, 1); grid.setColumnStretch(1, 1)
        outer.addLayout(grid)

        # Global row — 2x2 grid of preset buttons to stay within dock width.
        # Preset color intentionally ramps with duty magnitude (a data
        # encoding, not a safety class) so `bg=` stays literal here.
        preset_grid = QGridLayout(); preset_grid.setSpacing(3)
        presets = [(0, "#34495e"), (25, "#2980b9"), (50, "#f39c12"), (100, "#c0392b")]
        for idx, (pct, color) in enumerate(presets):
            b = QPushButton(f"All {pct}%"); _style_button(b, bg=color)
            b.setToolTip(f"Sends: SET_ALL_DUTY {pct / 100.0:.3f}")
            b.clicked.connect(lambda _, d=pct / 100.0: self._set_all(d))
            preset_grid.addWidget(b, idx // 2, idx % 2)
        clear = QPushButton("Clear Overrides"); _style_button(clear, cls="neutral")
        clear.setToolTip("Sends: CLEAR_OVERRIDES")
        clear.clicked.connect(lambda: self._send("CLEAR_OVERRIDES"))
        preset_grid.addWidget(clear, 2, 0, 1, 2)
        outer.addLayout(preset_grid)

        # Uniform slider
        ul = QHBoxLayout()
        self._uni = QSlider(Qt.Orientation.Horizontal); self._uni.setRange(0, 100)
        self._uni_lbl = QLabel("0%"); self._uni_lbl.setFixedWidth(34)
        self._uni.valueChanged.connect(lambda v: self._uni_lbl.setText(f"{v}%"))
        apply_btn = QPushButton("Apply All"); _style_button(apply_btn, cls="primary")
        apply_btn.setToolTip("Sends: SET_ALL_DUTY <duty>")
        apply_btn.clicked.connect(lambda: self._set_all(self._uni.value() / 100.0))
        ul.addWidget(QLabel("Uniform:")); ul.addWidget(self._uni); ul.addWidget(self._uni_lbl); ul.addWidget(apply_btn)
        outer.addLayout(ul)

        target_row = QHBoxLayout()
        target_row.addWidget(QLabel("All target:"))
        self._all_target = QDoubleSpinBox()
        self._all_target.setRange(0.0, 80.0)
        self._all_target.setDecimals(1)
        self._all_target.setValue(20.0)
        self._all_target.setSuffix(" C")
        set_all_target = QPushButton("Apply")
        _style_button(set_all_target, cls="success")
        set_all_target.setToolTip("Sends: SET_ALL_TEMP_TARGETS <target_c>")
        clear_all_target = QPushButton("Clear")
        _style_button(clear_all_target, cls="neutral")
        clear_all_target.setToolTip("Sends: CLEAR_TEMP_TARGETS")
        set_all_target.clicked.connect(self._set_all_target)
        clear_all_target.clicked.connect(
            lambda: self._send("CLEAR_TEMP_TARGETS")
        )
        target_row.addWidget(self._all_target, 1)
        target_row.addWidget(set_all_target)
        target_row.addWidget(clear_all_target)
        outer.addLayout(target_row)

        pid_row = QHBoxLayout()
        pid_row.addWidget(QLabel("PID:"))
        self._pid_channel = QComboBox()
        self._pid_channel.addItem("All", "ALL")
        for index in range(len(self._cells)):
            self._pid_channel.addItem(f"H{index}", str(index))
        pid_row.addWidget(self._pid_channel)
        self._kp = QDoubleSpinBox(); self._kp.setRange(0.0, 1000.0); self._kp.setDecimals(4); self._kp.setValue(0.20)
        self._ki = QDoubleSpinBox(); self._ki.setRange(0.0, 1000.0); self._ki.setDecimals(4); self._ki.setValue(0.02)
        self._kd = QDoubleSpinBox(); self._kd.setRange(0.0, 1000.0); self._kd.setDecimals(4); self._kd.setValue(0.03)
        for label, widget in (("Kp", self._kp), ("Ki", self._ki), ("Kd", self._kd)):
            pid_row.addWidget(QLabel(label)); pid_row.addWidget(widget)
        pid_btn = QPushButton("Apply")
        _style_button(pid_btn, cls="primary")
        pid_btn.setToolTip("Sends: SET_PID <channel> <kp> <ki> <kd>")
        pid_btn.clicked.connect(lambda: self._apply_pid())
        pid_row.addWidget(pid_btn)
        outer.addLayout(pid_row)

        profile_row = QHBoxLayout()
        self._profile_name = QLineEdit()
        self._profile_name.setPlaceholderText("profile name")
        self._profile_select = QComboBox()
        self._refresh_profile_names()
        save_profile = QPushButton("Save")
        apply_profile = QPushButton("Apply")
        _style_button(save_profile, cls="neutral")
        save_profile.setToolTip("Saves the profile locally — does not send a wire command.")
        _style_button(apply_profile, cls="success")
        apply_profile.setToolTip(
            "Sends: SET_PID ALL <kp> <ki> <kd>\nSends: SET_TEMP_TARGET <i> <target_c> (per heater)"
        )
        save_profile.clicked.connect(self._save_profile)
        apply_profile.clicked.connect(self._apply_profile)
        profile_row.addWidget(self._profile_name)
        profile_row.addWidget(self._profile_select)
        profile_row.addWidget(save_profile)
        profile_row.addWidget(apply_profile)
        outer.addLayout(profile_row)

    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def _set_one(self, idx: int, duty: float) -> None:
        ok_i, _ = validate_heater_index(idx)
        ok_d, d_norm = validate_duty(duty)
        if not (ok_i and ok_d):
            Toast.anchor(self, "invalid args", ok=False); return
        self._disp.send(f"SET_HEATER_DUTY {idx} {d_norm}", tag=self._cells[idx])

    def _set_all(self, duty: float) -> None:
        ok, d = validate_duty(duty)
        if not ok:
            Toast.anchor(self, "invalid duty", ok=False); return
        self._disp.send(f"SET_ALL_DUTY {d}", tag=self)

    def _set_target(self, idx: int, target_c: float) -> None:
        ok_i, _ = validate_heater_index(idx)
        ok_t, target = validate_temperature_target(target_c)
        if not (ok_i and ok_t):
            Toast.anchor(self, target, ok=False); return
        self._disp.send(f"SET_TEMP_TARGET {idx} {target}", tag=self._cells[idx])

    def _clear_target(self, idx: int) -> None:
        ok, _ = validate_heater_index(idx)
        if not ok:
            Toast.anchor(self, "invalid heater index", ok=False); return
        self._disp.send(f"CLEAR_TEMP_TARGET {idx}", tag=self._cells[idx])

    def _set_all_target(self) -> None:
        ok, target = validate_temperature_target(self._all_target.value())
        if not ok:
            Toast.anchor(self, target, ok=False); return
        for cell in self._cells:
            cell.set_target_value(self._all_target.value())
        self._disp.send(f"SET_ALL_TEMP_TARGETS {target}", tag=self)

    def _apply_pid(self, channel: Optional[str] = None) -> None:
        ok, gains = validate_pid_gains(
            self._kp.value(), self._ki.value(), self._kd.value()
        )
        if not ok:
            Toast.anchor(self, gains, ok=False); return
        target = channel or str(self._pid_channel.currentData())
        self._disp.send(f"SET_PID {target} {gains}", tag=self)

    def _refresh_profile_names(self) -> None:
        current = self._profile_select.currentText() if self._profile_select.count() else ""
        self._profile_select.clear()
        self._profile_select.addItems(sorted(self._profiles))
        if current in self._profiles:
            self._profile_select.setCurrentText(current)

    def _save_profile(self) -> None:
        name = self._profile_name.text().strip()
        if not name:
            Toast.anchor(self, "profile name required", ok=False); return
        self._profiles[name] = build_profile(
            [cell.target_value() for cell in self._cells],
            self._kp.value(), self._ki.value(), self._kd.value(),
        )
        try:
            save_profiles(self._profiles, self._profile_path)
        except OSError as exc:
            Toast.anchor(self, f"profile save failed: {exc}", ok=False); return
        self._refresh_profile_names()
        self._profile_select.setCurrentText(name)
        Toast.anchor(self, "profile saved", ok=True)

    def _apply_profile(self) -> None:
        name = self._profile_select.currentText()
        profile = self._profiles.get(name)
        if not profile:
            Toast.anchor(self, "select a profile", ok=False); return
        targets = profile.get("targets_c", [])
        pid = profile.get("pid", {})
        if len(targets) != len(self._cells):
            Toast.anchor(self, "profile heater count mismatch", ok=False); return
        try:
            kp, ki, kd = float(pid["kp"]), float(pid["ki"]), float(pid["kd"])
        except (KeyError, TypeError, ValueError):
            Toast.anchor(self, "invalid profile PID", ok=False); return
        self._kp.setValue(kp); self._ki.setValue(ki); self._kd.setValue(kd)
        self._pid_channel.setCurrentIndex(0)
        self._apply_pid("ALL")
        for idx, target in enumerate(targets):
            self._cells[idx].set_target_value(float(target))
            self._set_target(idx, float(target))

    def update_from_packet(self, pkt: TelemetryPacket) -> None:
        # Six cells (H0..H5, no box). Heater-to-sample mapping is
        # 1:1 for the first 6 samples; remaining samples (6, 7) are still
        # monitored but not actively heated.
        n = len(self._cells)
        duties = pkt.heater_duty + [0.0] * (n - len(pkt.heater_duty))
        temps = list(pkt.sample_temps_c)
        for i in range(n):
            t = (temps[i] if i < len(temps) and
                 pkt.sensor_valid.get(f"S{i}", True) and
                 math.isfinite(temps[i]) else None)
            self._cells[i].update_live(duties[i], t)


# Both motor ids on the wire. Single source of truth for "stop every
# motor" so Esc (main_window.py), the EmergencyBar's STOP MOTORS button,
# and StepperPanel.emergency_stop_all all generate the identical
# STEPPER_STOP <id> pair through the same CommandDispatcher.send() path.
STEPPER_MOTOR_IDS = (0, 1)


def _send_stepper_stop_all(dispatcher: CommandDispatcher, tag) -> None:
    for motor_id in STEPPER_MOTOR_IDS:
        dispatcher.send(f"STEPPER_STOP {motor_id}", tag=tag)


# ── Stepper ──────────────────────────────────────────────────────────────────
class StepperPanel(QGroupBox):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Stepper", parent)
        self._disp = dispatcher
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        # ── Motion section: motor select, live state, jog, direct move,
        # and the enable/disable/home/stop action row. ──
        motion_box = QGroupBox("Motion")
        motion = QVBoxLayout(motion_box); motion.setContentsMargins(6, 6, 6, 6); motion.setSpacing(4)

        motor_row = QHBoxLayout()
        motor_row.addWidget(QLabel("Motor:"))
        self._motor = QComboBox()
        self._motor.addItem("Motor 0", 0)
        self._motor.addItem("Motor 1", 1)
        motor_row.addWidget(self._motor)
        zero = QPushButton("SET ZERO"); _style_button(zero, cls="primary", bold=True)
        zero.setToolTip("Sends: SET_POSITION_ZERO <motor_id>")
        zero.clicked.connect(lambda: self._send(f"SET_POSITION_ZERO {self._motor_id()}", tag=zero))
        motor_row.addWidget(zero)
        motor_row.addStretch()
        motion.addLayout(motor_row)

        # State line
        self._state = QLabel("pos — / tgt — · — Hz · µ— · — · — · src=—")
        self._state.setStyleSheet(
            "background: #111; color: #eee; padding: 6px; border-radius: 3px; "
            "font-family: monospace; font-size: 10pt;"
        )
        motion.addWidget(self._state)

        # Jog: 4x2 grid so +/- groupings stay together and the row fits.
        jog = QGridLayout(); jog.setSpacing(3)
        for col, delta in enumerate((-1000, -100, -10, -1)):
            b = QPushButton(str(delta)); _style_button(b, cls="neutral", min_height=24)
            b.setToolTip(f"Sends: STEPPER_MOVE <motor_id> {delta}")
            b.clicked.connect(lambda _, d=delta: self._send(
                f"STEPPER_MOVE {self._motor_id()} {d}", tag=self))
            jog.addWidget(b, 0, col)
        for col, delta in enumerate((1, 10, 100, 1000)):
            b = QPushButton(f"+{delta}"); _style_button(b, cls="neutral", min_height=24)
            b.setToolTip(f"Sends: STEPPER_MOVE <motor_id> +{delta}")
            b.clicked.connect(lambda _, d=delta: self._send(
                f"STEPPER_MOVE {self._motor_id()} {d}", tag=self))
            jog.addWidget(b, 1, col)
        motion.addLayout(jog)

        # Direct input — one row per action keeps each clearly readable.
        def _add_input_row(label: str, spinbox: QWidget, btn_text: str, slot, tooltip: str) -> None:
            row = QHBoxLayout()
            lbl = QLabel(label); lbl.setFixedWidth(60)
            row.addWidget(lbl); row.addWidget(spinbox, 1)
            btn = QPushButton(btn_text); _style_button(btn, cls="primary")
            btn.setToolTip(tooltip)
            btn.clicked.connect(slot)
            row.addWidget(btn)
            motion.addLayout(row)

        self._move_in = QSpinBox(); self._move_in.setRange(-10_000_000, 10_000_000); self._move_in.setValue(100)
        _add_input_row("Δsteps:", self._move_in, "MOVE", self._on_move,
                       "Sends: STEPPER_MOVE <motor_id> <steps>")
        self._moveto_in = QSpinBox(); self._moveto_in.setRange(-10_000_000, 10_000_000)
        _add_input_row("abs pos:", self._moveto_in, "MOVETO", self._on_moveto,
                       "Sends: STEPPER_MOVETO <motor_id> <abs_usteps>")
        self._rot_in = QDoubleSpinBox(); self._rot_in.setRange(-100000.0, 100000.0); self._rot_in.setDecimals(3); self._rot_in.setValue(1.0)
        _add_input_row("revs:", self._rot_in, "ROTATE", self._on_rotate,
                       "Sends: STEPPER_ROTATE <motor_id> <revs>")

        # Enable/Disable/Home/Stop. DISABLE and STOP are danger-classed --
        # both cut motor output/motion, same tier as an emergency action.
        act = QHBoxLayout()
        en_btn  = QPushButton("ENABLE");  _style_button(en_btn,  cls="success", bold=True)
        en_btn.setToolTip("Sends: STEPPER_ENABLE <motor_id>")
        dis_btn = QPushButton("DISABLE"); _style_button(dis_btn, cls="danger")
        dis_btn.setToolTip("Sends: STEPPER_DISABLE <motor_id>")
        home    = QPushButton("HOME");    _style_button(home,    cls="primary", bold=True)
        home.setToolTip("Sends: STEPPER_HOME <motor_id>")
        self._stop = QPushButton("STOP"); _style_button(self._stop, cls="danger", bold=True, min_height=34)
        self._stop.setToolTip("Sends: STEPPER_STOP <motor_id> (selected motor only — Esc/STOP MOTORS stop both)")
        en_btn.clicked.connect(lambda: self._send(f"STEPPER_ENABLE {self._motor_id()}", tag=en_btn))
        dis_btn.clicked.connect(lambda: self._send(f"STEPPER_DISABLE {self._motor_id()}", tag=dis_btn))
        home.clicked.connect(lambda: self._send(f"STEPPER_HOME {self._motor_id()}", tag=home))
        self._stop.clicked.connect(self.emergency_stop)
        act.addWidget(en_btn); act.addWidget(dis_btn); act.addWidget(home); act.addWidget(self._stop)
        motion.addLayout(act)

        outer.addWidget(motion_box)

        # ── Config section: speed + microstep. ──
        config_box = QGroupBox("Config")
        config = QVBoxLayout(config_box); config.setContentsMargins(6, 6, 6, 6); config.setSpacing(4)

        speed_row = QHBoxLayout()
        speed_row.addWidget(QLabel("Speed:"))
        self._speed = QSlider(Qt.Orientation.Horizontal); self._speed.setRange(10, 4000); self._speed.setValue(400)
        self._speed_lbl = QLabel("400 Hz"); self._speed_lbl.setFixedWidth(60)
        self._speed.valueChanged.connect(lambda v: self._speed_lbl.setText(f"{v} Hz"))
        speed_btn = QPushButton("Set"); _style_button(speed_btn, cls="primary", min_height=22)
        speed_btn.setToolTip("Sends: STEPPER_SET_SPEED <motor_id> <hz>")
        speed_btn.clicked.connect(self._on_set_speed)
        speed_row.addWidget(self._speed, 1); speed_row.addWidget(self._speed_lbl); speed_row.addWidget(speed_btn)
        config.addLayout(speed_row)

        us_row = QHBoxLayout()
        us_row.addWidget(QLabel("µstep:"))
        self._us = QComboBox()
        self._us.addItems(["1", "2", "4", "8", "16", "32", "64", "128", "256"])
        self._us.setCurrentText("16")
        us_btn = QPushButton("Set"); _style_button(us_btn, cls="primary", min_height=22)
        us_btn.setToolTip("Sends: STEPPER_SET_MICROSTEP <motor_id> <n>")
        us_btn.clicked.connect(self._on_set_microstep)
        us_row.addWidget(self._us, 1); us_row.addWidget(us_btn); us_row.addStretch()
        config.addLayout(us_row)

        outer.addWidget(config_box)

        # ── Bend sequence section: collapsible (checkable QGroupBox),
        # collapsed by default -- biggest space hog here, rarely touched
        # mid-flight. Toggling the box show/hides `self._seq_body`, which
        # holds every widget in the section. ──
        self._seq_group = QGroupBox("Bend sequence")
        self._seq_group.setCheckable(True)
        self._seq_group.setChecked(False)
        self._seq_group.setToolTip(
            "Click to expand/collapse. Collapsed by default -- rarely used mid-flight."
        )
        seq_outer = QVBoxLayout(self._seq_group); seq_outer.setContentsMargins(6, 6, 6, 6); seq_outer.setSpacing(4)

        self._seq_body = QWidget()
        seq = QVBoxLayout(self._seq_body); seq.setContentsMargins(0, 0, 0, 0); seq.setSpacing(4)

        seq_header = QHBoxLayout()
        seq_header.addWidget(QLabel("Sequence:"))
        self._sequence_name = QLineEdit()
        self._sequence_name.setPlaceholderText("sequence name")
        seq_header.addWidget(self._sequence_name, 1)
        add_step = QPushButton("+"); _style_button(add_step, cls="neutral")
        add_step.setToolTip("Add sequence step")
        remove_step = QPushButton("-"); _style_button(remove_step, cls="neutral")
        remove_step.setToolTip("Remove selected sequence step")
        add_step.clicked.connect(self._add_sequence_step)
        remove_step.clicked.connect(self._remove_sequence_step)
        seq_header.addWidget(add_step); seq_header.addWidget(remove_step)
        seq.addLayout(seq_header)

        self._sequence = QTableWidget(0, 3)
        self._sequence.setHorizontalHeaderLabels(["Target usteps", "Hold s", "Speed Hz"])
        self._sequence.verticalHeader().setVisible(False)
        self._sequence.setMinimumHeight(120)
        self._sequence.horizontalHeader().setStretchLastSection(True)
        seq.addWidget(self._sequence)
        self._add_sequence_step()

        # RUN/RESUME are success (start motion); PAUSE/STOP are danger
        # (halt motion); LOAD only stores a definition (primary); STATUS
        # is a read-only query (neutral).
        seq_actions = QGridLayout(); seq_actions.setSpacing(3)
        for label, slot, cls_name, wire_tpl, row, col in (
            ("LOAD", self._load_sequence, "primary",
             "BENDSEQ_LOAD <motor_id> <name> <target>:<hold>[:<hz>] ...", 0, 0),
            ("RUN", self._run_sequence, "success", "BENDSEQ_RUN <motor_id> <name>", 0, 1),
            ("PAUSE", self._pause_sequence, "danger", "BENDSEQ_PAUSE <motor_id>", 0, 2),
            ("RESUME", self._resume_sequence, "success", "BENDSEQ_RESUME <motor_id>", 1, 0),
            ("STOP", self._stop_sequence, "danger", "BENDSEQ_STOP <motor_id>", 1, 1),
            ("STATUS", self._sequence_status, "neutral", "BENDSEQ_STATUS <motor_id>", 1, 2),
        ):
            button = QPushButton(label); _style_button(button, cls=cls_name, bold=True)
            button.setToolTip(f"Sends: {wire_tpl}")
            button.clicked.connect(slot)
            seq_actions.addWidget(button, row, col)
        seq.addLayout(seq_actions)

        seq_outer.addWidget(self._seq_body)
        self._seq_body.setVisible(self._seq_group.isChecked())
        self._seq_group.toggled.connect(self._seq_body.setVisible)
        outer.addWidget(self._seq_group)

    # ── helpers ──
    def _send(self, cmd: str, tag) -> None:
        self._disp.send(cmd, tag=tag)

    def _motor_id(self) -> int:
        return int(self._motor.currentData())

    def _on_move(self) -> None:
        ok, norm = validate_stepper_move(self._move_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_MOVE {self._motor_id()} {norm}", tag=self)

    def _on_moveto(self) -> None:
        ok, norm = validate_stepper_move(self._moveto_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_MOVETO {self._motor_id()} {norm}", tag=self)

    def _on_rotate(self) -> None:
        ok, norm = validate_revolutions(self._rot_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_ROTATE {self._motor_id()} {norm}", tag=self)

    def _on_set_speed(self) -> None:
        ok, norm = validate_speed_hz(self._speed.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_SET_SPEED {self._motor_id()} {norm}", tag=self)

    def _on_set_microstep(self) -> None:
        ok, norm = validate_microstep(int(self._us.currentText()))
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_SET_MICROSTEP {self._motor_id()} {norm}", tag=self)

    def _add_sequence_step(self) -> None:
        row = self._sequence.rowCount()
        self._sequence.insertRow(row)
        self._sequence.setItem(row, 0, QTableWidgetItem("0"))
        self._sequence.setItem(row, 1, QTableWidgetItem("0"))
        self._sequence.setItem(row, 2, QTableWidgetItem(""))

    def _remove_sequence_step(self) -> None:
        row = self._sequence.currentRow()
        if row < 0:
            row = self._sequence.rowCount() - 1
        if row >= 0:
            self._sequence.removeRow(row)

    def _sequence_identity(self) -> tuple[int, str] | None:
        name = self._sequence_name.text().strip()
        if not name or any(ch.isspace() for ch in name):
            Toast.anchor(self, "sequence name must be one word", ok=False)
            return None
        return self._motor_id(), name

    def _load_sequence(self) -> None:
        identity = self._sequence_identity()
        if identity is None:
            return
        if self._sequence.rowCount() == 0:
            Toast.anchor(self, "add at least one sequence step", ok=False); return
        encoded = []
        try:
            for row in range(self._sequence.rowCount()):
                target = int(self._sequence.item(row, 0).text().strip())
                hold = float(self._sequence.item(row, 1).text().strip())
                speed_text = self._sequence.item(row, 2).text().strip()
                if hold < 0:
                    raise ValueError("hold must be non-negative")
                step = f"{target}:{hold:g}"
                if speed_text:
                    speed = float(speed_text)
                    if speed <= 0:
                        raise ValueError("speed must be positive")
                    step += f":{speed:g}"
                encoded.append(step)
        except (AttributeError, TypeError, ValueError) as exc:
            Toast.anchor(self, f"invalid sequence step: {exc}", ok=False); return
        motor, name = identity
        self._disp.send(f"BENDSEQ_LOAD {motor} {name} {' '.join(encoded)}", tag=self)

    def _run_sequence(self) -> None:
        identity = self._sequence_identity()
        if identity:
            self._disp.send(f"BENDSEQ_RUN {identity[0]} {identity[1]}", tag=self)

    def _pause_sequence(self) -> None:
        self._disp.send(f"BENDSEQ_PAUSE {self._motor_id()}", tag=self)

    def _resume_sequence(self) -> None:
        self._disp.send(f"BENDSEQ_RESUME {self._motor_id()}", tag=self)

    def _stop_sequence(self) -> None:
        self._disp.send(f"BENDSEQ_STOP {self._motor_id()}", tag=self)

    def _sequence_status(self) -> None:
        self._disp.send(f"BENDSEQ_STATUS {self._motor_id()}", tag=self)

    def emergency_stop(self) -> None:
        """Stop only the currently-selected motor -- fired by this panel's
        own STOP button."""
        self._disp.send(f"STEPPER_STOP {self._motor_id()}", tag=self._stop)

    def emergency_stop_all(self) -> None:
        """Stop BOTH motors, regardless of which one is selected in the
        combo box. This is the panic-class path: Esc (main_window.py) and
        the EmergencyBar's STOP MOTORS button both route through this
        method (or its underlying `_send_stepper_stop_all` helper) so
        there is exactly one place that spells out what "stop both
        motors" means on the wire."""
        _send_stepper_stop_all(self._disp, self._stop)

    def sequence_expanded(self) -> bool:
        """Whether the collapsible bend-sequence section is currently
        expanded. Test-only accessor."""
        return self._seq_group.isChecked()

    def update_from_packet(self, pkt: TelemetryPacket) -> None:
        # Compact one-liner per motor. `pkt.steppers` is populated whenever
        # the onboard sends any stepper telemetry at all — indexed dual-motor
        # segments (STEPPER0=/STEPPER1=) synthesize one entry per motor, and
        # even the legacy single STEPPER= segment synthesizes a one-entry
        # list (see protocol.py's `parse_telemetry_csv`). So an empty list
        # means "no stepper telemetry yet", not "legacy frame".
        if not pkt.steppers:
            self._state.setText("pos — / tgt — · — Hz · µ— · — · — · src=—")
            return
        lines = []
        any_moving = False
        any_enabled = False
        for m in pkt.steppers:
            en  = "EN"  if m["enabled"] else "DIS"
            mv  = "MOVING" if m["moving"] else ("HOLD" if m["holding"] else "idle")
            any_moving |= bool(m["moving"])
            any_enabled |= bool(m["enabled"])
            hold_suffix = f" {m['hold_s']:.0f}s" if m["holding"] else ""
            lines.append(
                f"M{m['motor_id']}: pos {m['position']} / tgt {m['target']} · "
                f"{m['hz']:.0f} Hz · µ{m['microstep']} · {en} · {mv}"
                f"{hold_suffix} · src={m['source']}"
            )
        self._state.setText("\n".join(lines))
        color = ("#2ecc71" if (any_enabled and not any_moving)
                 else ("#f39c12" if any_moving else "#e74c3c"))
        self._state.setStyleSheet(
            f"background: #111; color: {color}; padding: 6px; border-radius: 3px; "
            "font-family: monospace; font-size: 10pt;"
        )


# ── Command / diagnostics ─────────────────────────────────────────────────────
class CommandPanel(QGroupBox):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Diagnostics", parent)
        self._disp = dispatcher
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        row = QGridLayout()
        b_ping = QPushButton("PING");   _style_button(b_ping,   cls="neutral"); b_ping.setToolTip("Sends: PING"); b_ping.clicked.connect(lambda: self._disp.send("PING",   tag=b_ping))
        b_stat = QPushButton("STATUS"); _style_button(b_stat,   cls="neutral"); b_stat.setToolTip("Sends: STATUS"); b_stat.clicked.connect(lambda: self._disp.send("STATUS", tag=b_stat))
        b_check = QPushButton("CHECK"); _style_button(b_check, cls="neutral"); b_check.setToolTip("Sends: CHECK"); b_check.clicked.connect(lambda: self._disp.send("CHECK", tag=b_check))
        b_components = QPushButton("COMPONENTS"); _style_button(b_components, cls="neutral"); b_components.setToolTip("Sends: COMPONENTS"); b_components.clicked.connect(lambda: self._disp.send("COMPONENTS", tag=b_components))
        row.addWidget(b_ping, 0, 0); row.addWidget(b_stat, 0, 1)
        row.addWidget(b_check, 0, 2); row.addWidget(b_components, 1, 0, 1, 2)
        b_reset = QPushButton("RESET_CTRL"); _style_button(b_reset, cls="danger")
        b_reset.setToolTip("Sends: RESET_CTRL")
        b_reset.clicked.connect(lambda: self._confirm_send(b_reset, "RESET_CTRL", "Reset controller?"))
        row.addWidget(b_reset, 1, 2)
        outer.addLayout(row)

        hz_row = QHBoxLayout()
        hz_row.addWidget(QLabel("Tick Hz:"))
        self._hz = QDoubleSpinBox(); self._hz.setRange(0.1, 5.0); self._hz.setDecimals(1); self._hz.setValue(1.0)
        hz_row.addWidget(self._hz)
        hz_btn = QPushButton("Set"); _style_button(hz_btn, cls="primary")
        hz_btn.setToolTip("Sends: SET_TICK_HZ <hz>")
        hz_btn.clicked.connect(self._on_set_hz)
        hz_row.addWidget(hz_btn)
        outer.addLayout(hz_row)

        # Arbitrary command — also how bench-only commands (ARM_DEBUG,
        # DISARM_DEBUG, HEATER_TEST, ...) reach the onboard; they don't get
        # dedicated buttons here.
        free_row = QHBoxLayout()
        self._free = QLineEdit()
        self._free.setPlaceholderText(
            "arbitrary command (e.g. ARM_DEBUG COATHEAL_DEBUG, HEATER_TEST 0 0.25)"
        )
        send_btn = QPushButton("Send"); _style_button(send_btn, cls="primary")
        send_btn.setToolTip("Sends: <command> (exactly what's typed, verbatim)")
        self._free.returnPressed.connect(lambda: self._fire_free(send_btn))
        send_btn.clicked.connect(lambda: self._fire_free(send_btn))
        free_row.addWidget(self._free); free_row.addWidget(send_btn)
        outer.addLayout(free_row)

    def _confirm_send(self, tag, cmd: str, title: str) -> None:
        if confirm(self, title, f"Send {cmd}?"):
            self._disp.send(cmd, tag=tag)

    def _on_set_hz(self) -> None:
        ok, norm = validate_tick_hz(self._hz.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"SET_TICK_HZ {norm}", tag=self)

    def _fire_free(self, tag) -> None:
        cmd = self._free.text().strip()
        if not cmd:
            return
        self._disp.send(cmd, tag=tag)
        self._free.clear()


# ── Emergency bar ────────────────────────────────────────────────────────────
class EmergencyBar(QWidget):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self._disp = dispatcher
        self.setStyleSheet("background: #2b0000;")
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 4, 6, 4); outer.setSpacing(4)

        warn = QLabel("⚠ EMERGENCY — click to fire")
        warn.setStyleSheet("color: #ff6b6b; font-weight: bold; font-size: 11pt; background: transparent;")
        outer.addWidget(warn)

        btn_row = QGridLayout(); btn_row.setSpacing(4)

        # `_PANIC_RED` is a deliberately darker shade than the standard
        # "danger" class -- it marks the two unconfirmed panic buttons
        # (HEATERS OFF, STOP MOTORS) as visually distinct from the
        # confirm-gated danger actions beside them.
        _PANIC_RED = "#8e1d1d"

        def add(label: str, wire: str, needs_confirm: bool, pos: tuple[int, int],
                *, bg: Optional[str] = None) -> QPushButton:
            b = QPushButton(label)
            _style_button(b, cls="danger", bg=bg, bold=True, min_height=30)
            b.setToolTip(f"Sends: {wire}")
            def fire():
                if needs_confirm and not confirm(self, label, f"Send {wire}?"):
                    return
                self._disp.send(wire, tag=b)
            b.clicked.connect(fire)
            btn_row.addWidget(b, pos[0], pos[1])
            return b

        # HEATERS_OFF and STOP MOTORS have no confirmation — they are the
        # panic buttons, same policy: fire immediately, no dialog in the way.
        add("HEATERS OFF",   "HEATERS_OFF",   False, (0, 0), bg=_PANIC_RED)
        self._stop_motors_btn = QPushButton("STOP MOTORS")
        _style_button(self._stop_motors_btn, cls="danger", bg=_PANIC_RED, bold=True, min_height=30)
        self._stop_motors_btn.setToolTip(
            f"Sends: STEPPER_STOP {STEPPER_MOTOR_IDS[0]}\nSends: STEPPER_STOP {STEPPER_MOTOR_IDS[1]}"
        )
        self._stop_motors_btn.clicked.connect(
            lambda: _send_stepper_stop_all(self._disp, self._stop_motors_btn)
        )
        btn_row.addWidget(self._stop_motors_btn, 0, 1)
        add("ENTER SAFE",    "ENTER_SAFE",    True,  (1, 0))
        add("SHUTDOWN SAFE", "SHUTDOWN_SAFE", True,  (1, 1))
        add("RADIO SILENCE", "RADIO_SILENCE", True,  (1, 2))
        for col in range(3):
            btn_row.setColumnStretch(col, 1)
        outer.addLayout(btn_row)
