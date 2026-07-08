"""Control panels that sit in the left dock.

Sections:
  * ConnectionPanel — host/port + Start button + status line.
  * ModePanel      — big state tile + ARM/DISARM/SAFE/RADIO rows.
  * HeaterPanel    — 5x2 grid of HeaterCell with global presets.
  * StepperPanel   — state line, jog row, direct input, config, BEND presets.
  * CommandPanel   — diagnostics (PING/STATUS/TICK_HZ) + ARM_DEBUG + arbitrary
                     command entry.
  * EmergencyBar   — always-visible red bar of panic actions.

Every button uses CommandDispatcher.send() with a tag so toasts anchor
correctly.
"""
from __future__ import annotations

from typing import Callable, Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QComboBox, QDoubleSpinBox, QFormLayout, QFrame, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QProgressBar, QPushButton, QScrollArea,
    QSizePolicy, QSlider, QSpinBox, QVBoxLayout, QWidget,
)

from ..protocol import (
    CommandResponse, StepperSnapshot, TelemetryPacket,
    validate_duty, validate_heater_index, validate_microstep,
    validate_revolutions, validate_speed_hz, validate_stepper_move,
    validate_tick_hz,
)
from .dispatch import CommandDispatcher
from .theme import HEATER_COLORS, HEATER_LABELS, mode_color, phase_color
from .widgets import StatusDot, Toast, confirm


def _style_button(btn: QPushButton, *, bg: str, fg: str = "white",
                  bold: bool = False, min_height: int = 26) -> None:
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
        _style_button(self._start_btn, bg="#2980b9", bold=True, min_height=30)
        self._start_btn.clicked.connect(self._emit_start)
        form.addRow(self._start_btn)

        self._status = QLabel("● idle")
        self._status.setStyleSheet("color: #888; font-size: 11px;")
        form.addRow(self._status)

        self._discovered = QLabel("Discovered: —")
        self._discovered.setStyleSheet("color: #888; font-family: monospace; font-size: 10px;")
        self._discovered.setWordWrap(True)
        form.addRow(self._discovered)

    def _emit_start(self) -> None:
        self.start_requested.emit(self._bind.text().strip(), self._tel_port.value(),
                                  self._cmd_port.value(), self._cmd_host.text().strip())
        self._start_btn.setEnabled(False)
        self._start_btn.setText("■ Receiver running")

    def set_connected(self, connected: bool, addr: str) -> None:
        if connected:
            self._status.setText(f"● connected {addr}")
            self._status.setStyleSheet("color: #2ecc71; font-size: 11px;")
        else:
            self._status.setText("● waiting for onboard…")
            self._status.setStyleSheet("color: #f39c12; font-size: 11px;")

    def set_status(self, state: str) -> None:
        colors = {
            "listening": ("#3498db", "● listening"),
            "connected": ("#2ecc71", "● connected"),
            "stale":     ("#f39c12", "● stale (no data)"),
            "searching": ("#f39c12", "● searching for onboard…"),
        }
        color, text = colors.get(state, ("#888", f"● {state}"))
        self._status.setText(text)
        self._status.setStyleSheet(f"color: {color}; font-size: 11px;")

    def set_discovered(self, host: str, cmd_port: int, tel_port: int,
                       session: str, hostname: str) -> None:
        self._discovered.setText(
            f"Discovered: {hostname}@{host}  cmd:{cmd_port} tel:{tel_port}  sess:{session[:8]}"
        )
        self._discovered.setStyleSheet(
            "color: #2ecc71; font-family: monospace; font-size: 10px;"
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
            phase_row, "SET PHASE", "SET_PHASE BOOT", "#2980b9",
            confirm_title="Set mission phase?")

        # Rows
        row1 = QHBoxLayout(); outer.addLayout(row1)
        self._btn_arm   = self._mk_cmd(row1, "ARM",         "ARM",        "#27ae60", confirm_title="Arm experiment?")
        self._btn_dis   = self._mk_cmd(row1, "DISARM",      "DISARM",     "#7f8c8d")
        self._btn_start = self._mk_cmd(row1, "FORCE START", "FORCE_START", "#2980b9")
        self._btn_stop  = self._mk_cmd(row1, "FORCE STOP",  "FORCE_STOP",  "#c0392b", confirm_title="Force stop?")

        row2 = QHBoxLayout(); outer.addLayout(row2)
        self._btn_safe_in  = self._mk_cmd(row2, "ENTER SAFE",      "ENTER_SAFE",      "#e67e22", confirm_title="Enter SAFE mode?")
        self._btn_safe_out = self._mk_cmd(row2, "EXIT SAFE",       "EXIT_SAFE",       "#7f8c8d")
        self._btn_second   = self._mk_cmd(row2, "SECONDARY CYCLE", "SECONDARY_CYCLE", "#8e44ad", confirm_title="Request secondary cycle?")

        row3 = QHBoxLayout(); outer.addLayout(row3)
        self._btn_radio_off = self._mk_cmd(row3, "RADIO SILENCE", "RADIO_SILENCE", "#c0392b", confirm_title="Silence downlink?")
        self._btn_radio_on  = self._mk_cmd(row3, "RADIO RESUME",  "RADIO_RESUME",  "#27ae60")
        self._btn_radio_on.setEnabled(False)

    def _mk_cmd(self, layout: QHBoxLayout, label: str, wire_cmd: str, color: str,
                *, confirm_title: Optional[str] = None) -> QPushButton:
        btn = QPushButton(label)
        btn.setToolTip(f"Sends: {wire_cmd}")
        _style_button(btn, bg=color, bold=True)
        btn.clicked.connect(lambda: self._fire(btn, wire_cmd, confirm_title))
        layout.addWidget(btn)
        return btn

    def _fire(self, btn: QPushButton, wire_cmd: str, confirm_title: Optional[str]) -> None:
        if confirm_title is not None and not confirm(self, confirm_title, f"Send {wire_cmd}?"):
            return
        if wire_cmd == "RADIO_SILENCE":
            self._radio_silent = True
            self._btn_radio_on.setEnabled(True)
        elif wire_cmd == "RADIO_RESUME":
            self._radio_silent = False
            self._btn_radio_on.setEnabled(False)
        elif wire_cmd.startswith("SET_PHASE "):
            wire_cmd = f"SET_PHASE {self._phase_select.currentText()}"
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
        self._readout.setStyleSheet("font-family: monospace; font-size: 10px; color: #bbb;")
        lay.addWidget(self._readout)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, 100); self._slider.setSingleStep(5); self._slider.setPageStep(10)
        self._slider.setTickPosition(QSlider.TickPosition.NoTicks)
        self._slider.valueChanged.connect(self._on_slider_changed)
        lay.addWidget(self._slider)

        btns = QHBoxLayout()
        self._slider_val = QLabel("0%"); self._slider_val.setFixedWidth(34)
        self._slider_val.setStyleSheet("font-family: monospace; font-size: 10px;")
        btns.addWidget(self._slider_val)
        set_btn = QPushButton("Set"); _style_button(set_btn, bg="#2980b9", min_height=22)
        off_btn = QPushButton("Off"); _style_button(off_btn, bg="#34495e", min_height=22)
        set_btn.clicked.connect(lambda: self.set_requested.emit(self.idx, self._slider.value() / 100.0))
        off_btn.clicked.connect(lambda: self.off_requested.emit(self.idx))
        btns.addWidget(set_btn); btns.addWidget(off_btn)
        lay.addLayout(btns)

    def _style_bar(self, color: str) -> None:
        self._bar.setStyleSheet(
            "QProgressBar { border: 1px solid #333; border-radius: 2px; background: #0e0e0e; }"
            f"QProgressBar::chunk {{ background-color: {color}; border-radius: 1px; }}"
        )

    def _on_slider_changed(self, v: int) -> None:
        self._slider_val.setText(f"{v}%")

    def set_enabled_controls(self, enabled: bool) -> None:
        self._slider.setEnabled(enabled)

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

        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        self._banner = QLabel("Manual duty controls")
        self._banner.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._banner.setStyleSheet(
            "background: #1a2d3a; color: #8fd3ff; padding: 4px; border-radius: 3px; "
            "font-weight: bold; font-size: 10pt;"
        )
        outer.addWidget(self._banner)

        # Rev-B.1: 6 heater cells (H0..H5). Box heater has been removed
        # from the flight hardware. Laid out 3x2 (two columns).
        grid = QGridLayout(); grid.setSpacing(4)
        self._cells: list[HeaterCell] = []
        num_cells = len(HEATER_LABELS)
        for i in range(num_cells):
            cell = HeaterCell(i)
            cell.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
            cell.set_requested.connect(self._set_one)
            cell.off_requested.connect(lambda idx: self._set_one(idx, 0.0))
            grid.addWidget(cell, i // 2, i % 2)
            self._cells.append(cell)
        grid.setColumnStretch(0, 1); grid.setColumnStretch(1, 1)
        outer.addLayout(grid)

        # Global row — 2x2 grid of preset buttons to stay within dock width.
        preset_grid = QGridLayout(); preset_grid.setSpacing(3)
        presets = [(0, "#34495e"), (25, "#2980b9"), (50, "#f39c12"), (100, "#c0392b")]
        for idx, (pct, color) in enumerate(presets):
            b = QPushButton(f"All {pct}%"); _style_button(b, bg=color)
            b.clicked.connect(lambda _, d=pct / 100.0: self._set_all(d))
            preset_grid.addWidget(b, idx // 2, idx % 2)
        clear = QPushButton("Clear Overrides"); _style_button(clear, bg="#7f8c8d")
        clear.clicked.connect(lambda: self._send("CLEAR_OVERRIDES"))
        preset_grid.addWidget(clear, 2, 0, 1, 2)
        outer.addLayout(preset_grid)

        # Uniform slider
        ul = QHBoxLayout()
        self._uni = QSlider(Qt.Orientation.Horizontal); self._uni.setRange(0, 100)
        self._uni_lbl = QLabel("0%"); self._uni_lbl.setFixedWidth(34)
        self._uni.valueChanged.connect(lambda v: self._uni_lbl.setText(f"{v}%"))
        apply_btn = QPushButton("Apply All"); _style_button(apply_btn, bg="#2980b9")
        apply_btn.clicked.connect(lambda: self._set_all(self._uni.value() / 100.0))
        ul.addWidget(QLabel("Uniform:")); ul.addWidget(self._uni); ul.addWidget(self._uni_lbl); ul.addWidget(apply_btn)
        outer.addLayout(ul)

        self.set_armed(True)

    def set_armed(self, _armed: bool) -> None:
        self._banner.setText("Manual duty controls")
        self._banner.setStyleSheet(
            "background: #1a2d3a; color: #8fd3ff; padding: 4px; border-radius: 3px; "
            "font-weight: bold; font-size: 10pt;"
        )
        for c in self._cells:
            c.set_enabled_controls(True)

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

    def update_from_packet(self, pkt: TelemetryPacket) -> None:
        # Rev-B.1: 6 cells (H0..H5, no box). Heater-to-sample mapping is
        # 1:1 for the first 6 samples; remaining samples (6, 7) are still
        # monitored but not actively heated.
        n = len(self._cells)
        duties = pkt.heater_duty + [0.0] * (n - len(pkt.heater_duty))
        temps = list(pkt.sample_temps_c)
        for i in range(n):
            t = temps[i] if i < len(temps) else None
            self._cells[i].update_live(duties[i], t)


# ── Stepper ──────────────────────────────────────────────────────────────────
class StepperPanel(QGroupBox):
    BEND_PRESETS = [("Ascent", "ASCENT"), ("Pre-Float", "PRE_FLOAT"),
                    ("Float", "FLOAT"), ("Descent", "DESCENT")]

    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Stepper", parent)
        self._disp = dispatcher
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        # State line
        self._state = QLabel("pos — / tgt — · — Hz · µ— · — · — · src=—")
        self._state.setStyleSheet(
            "background: #111; color: #eee; padding: 6px; border-radius: 3px; "
            "font-family: monospace; font-size: 10pt;"
        )
        outer.addWidget(self._state)

        # Jog: 4x2 grid so +/- groupings stay together and the row fits.
        jog = QGridLayout(); jog.setSpacing(3)
        for col, delta in enumerate((-1000, -100, -10, -1)):
            b = QPushButton(str(delta)); _style_button(b, bg="#34495e", min_height=24)
            b.clicked.connect(lambda _, d=delta: self._send(f"STEPPER_MOVE {d}", tag=self))
            jog.addWidget(b, 0, col)
        for col, delta in enumerate((1, 10, 100, 1000)):
            b = QPushButton(f"+{delta}"); _style_button(b, bg="#34495e", min_height=24)
            b.clicked.connect(lambda _, d=delta: self._send(f"STEPPER_MOVE {d}", tag=self))
            jog.addWidget(b, 1, col)
        outer.addLayout(jog)

        # Direct input — one row per action keeps each clearly readable.
        def _add_input_row(label: str, spinbox: QWidget, btn_text: str, slot) -> None:
            row = QHBoxLayout()
            lbl = QLabel(label); lbl.setFixedWidth(60)
            row.addWidget(lbl); row.addWidget(spinbox, 1)
            btn = QPushButton(btn_text); _style_button(btn, bg="#2980b9")
            btn.clicked.connect(slot)
            row.addWidget(btn)
            outer.addLayout(row)

        self._move_in = QSpinBox(); self._move_in.setRange(-10_000_000, 10_000_000); self._move_in.setValue(100)
        _add_input_row("Δsteps:", self._move_in, "MOVE", self._on_move)
        self._moveto_in = QSpinBox(); self._moveto_in.setRange(-10_000_000, 10_000_000)
        _add_input_row("abs pos:", self._moveto_in, "MOVETO", self._on_moveto)
        self._rot_in = QDoubleSpinBox(); self._rot_in.setRange(-100000.0, 100000.0); self._rot_in.setDecimals(3); self._rot_in.setValue(1.0)
        _add_input_row("revs:", self._rot_in, "ROTATE", self._on_rotate)

        # Config: speed row + microstep row (two rows avoid overflow).
        speed_row = QHBoxLayout()
        speed_row.addWidget(QLabel("Speed:"))
        self._speed = QSlider(Qt.Orientation.Horizontal); self._speed.setRange(10, 4000); self._speed.setValue(400)
        self._speed_lbl = QLabel("400 Hz"); self._speed_lbl.setFixedWidth(60)
        self._speed.valueChanged.connect(lambda v: self._speed_lbl.setText(f"{v} Hz"))
        speed_btn = QPushButton("Set"); _style_button(speed_btn, bg="#2980b9", min_height=22)
        speed_btn.clicked.connect(self._on_set_speed)
        speed_row.addWidget(self._speed, 1); speed_row.addWidget(self._speed_lbl); speed_row.addWidget(speed_btn)
        outer.addLayout(speed_row)

        us_row = QHBoxLayout()
        us_row.addWidget(QLabel("µstep:"))
        self._us = QComboBox(); self._us.addItems(["1", "2", "4", "8", "16", "32"]); self._us.setCurrentText("16")
        us_btn = QPushButton("Set"); _style_button(us_btn, bg="#2980b9", min_height=22)
        us_btn.clicked.connect(self._on_set_microstep)
        us_row.addWidget(self._us, 1); us_row.addWidget(us_btn); us_row.addStretch()
        outer.addLayout(us_row)

        # Enable/Disable/Home/Stop
        act = QHBoxLayout()
        en_btn  = QPushButton("ENABLE");  _style_button(en_btn,  bg="#27ae60", bold=True)
        dis_btn = QPushButton("DISABLE"); _style_button(dis_btn, bg="#7f8c8d")
        home    = QPushButton("HOME");    _style_button(home,    bg="#2980b9", bold=True)
        self._stop = QPushButton("STOP"); _style_button(self._stop, bg="#c0392b", bold=True, min_height=34)
        en_btn.clicked.connect(lambda: self._send("STEPPER_ENABLE", tag=en_btn))
        dis_btn.clicked.connect(lambda: self._send("STEPPER_DISABLE", tag=dis_btn))
        home.clicked.connect(lambda: self._send("STEPPER_HOME", tag=home))
        self._stop.clicked.connect(self.emergency_stop)
        act.addWidget(en_btn); act.addWidget(dis_btn); act.addWidget(home); act.addWidget(self._stop)
        outer.addLayout(act)

        # BEND presets — 2x2 grid.
        bend = QGridLayout(); bend.setSpacing(4)
        self._bend_hold: dict[str, QDoubleSpinBox] = {}
        for idx, (label, phase) in enumerate(self.BEND_PRESETS):
            w = QWidget(); vl = QVBoxLayout(w); vl.setContentsMargins(0, 0, 0, 0); vl.setSpacing(2)
            b = QPushButton(f"BEND {label}"); _style_button(b, bg="#8e44ad", bold=True, min_height=36)
            b.setToolTip(f"Sends: STEPPER_BEND  (target from config for {phase})")
            hold = QDoubleSpinBox(); hold.setRange(0.0, 36000.0); hold.setSuffix(" s hold"); hold.setDecimals(0)
            self._bend_hold[phase] = hold
            b.clicked.connect(lambda _, p=phase: self._on_bend(p))
            vl.addWidget(b); vl.addWidget(hold)
            bend.addWidget(w, idx // 2, idx % 2)
        outer.addLayout(bend)

    # ── helpers ──
    def _send(self, cmd: str, tag) -> None:
        self._disp.send(cmd, tag=tag)

    def _on_move(self) -> None:
        ok, norm = validate_stepper_move(self._move_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_MOVE {norm}", tag=self)

    def _on_moveto(self) -> None:
        ok, norm = validate_stepper_move(self._moveto_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_MOVETO {norm}", tag=self)

    def _on_rotate(self) -> None:
        ok, norm = validate_revolutions(self._rot_in.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_ROTATE {norm}", tag=self)

    def _on_set_speed(self) -> None:
        ok, norm = validate_speed_hz(self._speed.value())
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_SET_SPEED {norm}", tag=self)

    def _on_set_microstep(self) -> None:
        ok, norm = validate_microstep(int(self._us.currentText()))
        if not ok:
            Toast.anchor(self, norm, ok=False); return
        self._disp.send(f"STEPPER_SET_MICROSTEP {norm}", tag=self)

    def _on_bend(self, phase: str) -> None:
        hold = int(self._bend_hold[phase].value())
        # Onboard resolves the bend target from config based on current phase;
        # we expose BEND as: STEPPER_BEND <target_steps> [hold_s]. Without a
        # known target here, we send target=0 and the operator overrides via
        # the hold spinbox. A future enhancement can read config from onboard.
        if hold > 0:
            self._disp.send(f"STEPPER_BEND 0 {hold}", tag=self)
        else:
            self._disp.send("STEPPER_BEND 0", tag=self)

    def emergency_stop(self) -> None:
        self._disp.send("STEPPER_STOP", tag=self._stop)

    def update_from_packet(self, pkt: TelemetryPacket) -> None:
        # Rev-B: show a compact one-liner per motor if dual; otherwise fall
        # back to the legacy single-motor rendering via `pkt.stepper`.
        if pkt.steppers:
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
            return

        s = pkt.stepper
        if s is None:
            self._state.setText("pos — / tgt — · — Hz · µ— · — · — · src=—")
            return
        en  = "EN"  if s.enabled else "DIS"
        mv  = "MOVING" if s.moving else ("HOLD" if s.holding else "idle")
        text = (f"pos {s.position} / tgt {s.target} · {s.hz:.0f} Hz · µ{s.microstep} · "
                f"{en} · {mv}{f' {s.hold_s:.0f}s' if s.holding else ''} · src={s.source}")
        self._state.setText(text)
        color = "#2ecc71" if s.enabled and not s.moving else "#f39c12" if s.moving else "#e74c3c"
        self._state.setStyleSheet(
            f"background: #111; color: {color}; padding: 6px; border-radius: 3px; "
            "font-family: monospace; font-size: 10pt;"
        )


# ── Command / diagnostics ─────────────────────────────────────────────────────
class CommandPanel(QGroupBox):
    debug_armed_changed = pyqtSignal(bool)

    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__("Diagnostics", parent)
        self._disp = dispatcher
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(4)

        row = QHBoxLayout()
        b_ping = QPushButton("PING");   _style_button(b_ping,   bg="#2980b9"); b_ping.clicked.connect(lambda: self._disp.send("PING",   tag=b_ping))
        b_stat = QPushButton("STATUS"); _style_button(b_stat,   bg="#2980b9"); b_stat.clicked.connect(lambda: self._disp.send("STATUS", tag=b_stat))
        row.addWidget(b_ping); row.addWidget(b_stat)
        b_reset = QPushButton("RESET_CTRL"); _style_button(b_reset, bg="#e67e22")
        b_reset.clicked.connect(lambda: self._confirm_send(b_reset, "RESET_CTRL", "Reset controller?"))
        row.addWidget(b_reset)
        outer.addLayout(row)

        hz_row = QHBoxLayout()
        hz_row.addWidget(QLabel("Tick Hz:"))
        self._hz = QDoubleSpinBox(); self._hz.setRange(0.1, 5.0); self._hz.setDecimals(1); self._hz.setValue(1.0)
        hz_row.addWidget(self._hz)
        hz_btn = QPushButton("Set"); _style_button(hz_btn, bg="#2980b9")
        hz_btn.clicked.connect(self._on_set_hz)
        hz_row.addWidget(hz_btn)
        outer.addLayout(hz_row)

        # ARM_DEBUG
        arm_row = QHBoxLayout()
        arm_row.addWidget(QLabel("Token:"))
        self._token = QLineEdit(); self._token.setEchoMode(QLineEdit.EchoMode.Password)
        self._token.setPlaceholderText("COATHEAL_DEBUG")
        arm_row.addWidget(self._token)
        arm_btn = QPushButton("ARM_DEBUG"); _style_button(arm_btn, bg="#c0392b", bold=True)
        dis_btn = QPushButton("DISARM");    _style_button(dis_btn, bg="#7f8c8d")
        arm_btn.clicked.connect(self._on_arm_debug)
        dis_btn.clicked.connect(self._on_disarm_debug)
        arm_row.addWidget(arm_btn); arm_row.addWidget(dis_btn)
        outer.addLayout(arm_row)

        # Arbitrary command
        free_row = QHBoxLayout()
        self._free = QLineEdit(); self._free.setPlaceholderText("arbitrary command (e.g. SET_ALL_DUTY 0.25)")
        send_btn = QPushButton("Send"); _style_button(send_btn, bg="#2c3e50")
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

    def _on_arm_debug(self) -> None:
        tok = self._token.text().strip() or "COATHEAL_DEBUG"
        self._disp.send(f"ARM_DEBUG {tok}", tag=self)
        self.debug_armed_changed.emit(True)

    def _on_disarm_debug(self) -> None:
        self._disp.send("DISARM_DEBUG", tag=self)
        self.debug_armed_changed.emit(False)

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

        def add(label: str, wire: str, needs_confirm: bool, color: str, pos: tuple[int, int]) -> None:
            b = QPushButton(label); _style_button(b, bg=color, bold=True, min_height=30)
            b.setToolTip(f"Sends: {wire}")
            def fire():
                if needs_confirm and not confirm(self, label, f"Send {wire}?"):
                    return
                self._disp.send(wire, tag=b)
            b.clicked.connect(fire)
            btn_row.addWidget(b, pos[0], pos[1])

        # HEATERS_OFF has no confirmation — it is the panic button.
        add("HEATERS OFF",   "HEATERS_OFF",   False, "#8e1d1d", (0, 0))
        add("ENTER SAFE",    "ENTER_SAFE",    True,  "#c0392b", (0, 1))
        add("SHUTDOWN SAFE", "SHUTDOWN_SAFE", True,  "#c0392b", (0, 2))
        add("RADIO SILENCE", "RADIO_SILENCE", True,  "#c0392b", (0, 3))
        for col in range(4):
            btn_row.setColumnStretch(col, 1)
        outer.addLayout(btn_row)
