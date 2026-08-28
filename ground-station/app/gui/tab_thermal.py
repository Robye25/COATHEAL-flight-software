"""Thermal tab: budget header, six heater rows, all-channel targets,
presets (redesign spec §5.3). PID tuning and open-loop duty live in the
Advanced tab."""
from __future__ import annotations

from typing import Dict, List, Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QComboBox, QDoubleSpinBox, QGridLayout, QHBoxLayout, QInputDialog, QLabel,
    QProgressBar, QScrollArea, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, validate_temperature_target
from ..reply_format import parse_kv_body
from ..thermal_presets import PresetStore, ThermalPreset
from . import gating
from .dispatch import CommandDispatcher
from .state import HEATER_COUNT, HEATER_SAMPLE, OnboardState
from .theme import HEATER_COLORS
from .widgets import (
    AMBER, GREEN, MONO_CSS, MUTED, RED, Indicator, ResponseLine, group_box, hrow, make_button,
)

DEFAULT_TARGET_C = 20.0


def heater_state(duty: float, target: Optional[float], temp_valid: bool, inhibited: bool) -> str:
    """State word for one heater row (spec §5.3)."""
    if not temp_valid:
        return "NO TEMP"
    if inhibited:
        return "INHIBITED"
    if duty > 0.0 and target is not None:
        return "PID"
    if duty > 0.0:
        return "DUTY"
    if target is not None:
        return "PID"
    return "OFF"


_STATE_COLORS = {"NO TEMP": RED, "INHIBITED": AMBER, "PID": GREEN, "DUTY": "#3498db", "OFF": MUTED}
_STATE_SHORT = {"NO TEMP": "NO-T", "INHIBITED": "INHIB"}


class HeaterRow(QWidget):
    def __init__(self, index: int, tab: "ThermalTab", parent=None):
        super().__init__(parent)
        self.index = index
        self._tab = tab
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(4)
        name = QLabel(f"H{index}")
        name.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {HEATER_COLORS[index % len(HEATER_COLORS)]};")
        name.setFixedWidth(22)
        self.measured = QLabel("—")
        self.measured.setStyleSheet(MONO_CSS)
        self.measured.setFixedWidth(64)
        self.target = QDoubleSpinBox()
        self.target.setRange(0.0, 80.0); self.target.setDecimals(1); self.target.setValue(DEFAULT_TARGET_C)
        self.target.setSuffix("°C"); self.target.setFixedWidth(62)
        self.target.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.btn_set = make_button("Set", "primary", sends=f"SET_TEMP_TARGET {index} <target_c>", min_height=22,
                                   compact=True, width=32, slot=lambda: tab.set_target(index, self.target.value()))
        self.btn_clear = make_button("Clr", "neutral", sends=f"CLEAR_TEMP_TARGET {index}", min_height=22,
                                     compact=True, width=32, slot=lambda: tab.clear_target(index))
        self.bar = QProgressBar(); self.bar.setRange(0, 1000); self.bar.setTextVisible(False)
        self.bar.setFixedHeight(9); self.bar.setMinimumWidth(28)
        self._style_bar(MUTED)
        self.duty = QLabel("  0 %"); self.duty.setStyleSheet(MONO_CSS); self.duty.setFixedWidth(36)
        self.state = QLabel("—"); self.state.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {MUTED};")
        self.state.setFixedWidth(40)
        for w in (name, self.measured, self.target, self.btn_set, self.btn_clear):
            lay.addWidget(w)
        lay.addWidget(self.bar, 1)
        lay.addWidget(self.duty)
        lay.addWidget(self.state)

    def _style_bar(self, color: str) -> None:
        self.bar.setStyleSheet(
            "QProgressBar { border: 1px solid #333; border-radius: 2px; background: #0e0e0e; }"
            f"QProgressBar::chunk {{ background-color: {color}; }}")

    def update_row(self, temp: Optional[float], duty: float, target: Optional[float],
                   inhibited: bool, reason: Optional[str]) -> None:
        sample = HEATER_SAMPLE[self.index]
        if temp is None:
            self.measured.setText(f"S{sample} —")
            self.measured.setStyleSheet(f"{MONO_CSS} color: {RED};")
            self.measured.setToolTip(f"S{sample}: no valid temperature")
        else:
            self.measured.setText(f"S{sample} {temp:5.1f}")
            self.measured.setStyleSheet(MONO_CSS)
            self.measured.setToolTip(f"S{sample} = {temp:.2f} °C")
        pct = max(0.0, min(1.0, duty)) * 100.0
        self.bar.setValue(int(pct * 10))
        self._style_bar(GREEN if pct < 60 else AMBER if pct < 85 else RED)
        self.duty.setText(f"{pct:3.0f} %")
        word = heater_state(duty, target, temp is not None, inhibited)
        self.state.setText(_STATE_SHORT.get(word, word))
        self.state.setToolTip(word)
        self.state.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {_STATE_COLORS[word]};")
        self.btn_set.set_reason(reason)
        self.btn_clear.set_reason(gating.generic_reason(self._tab.state))


class ThermalTab(QScrollArea):
    targets_changed = pyqtSignal(list)

    def __init__(self, dispatcher: CommandDispatcher, preset_store: Optional[PresetStore] = None, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self.state = OnboardState()
        self._targets: List[Optional[float]] = [None] * HEATER_COUNT   # last known onboard targets
        self._limits = (0.0, 80.0)
        self._gains = (0.20, 0.02, 0.03)
        self.presets = preset_store if preset_store is not None else PresetStore().load()

        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        frame, lay = group_box("Budget and control")
        self.energy_bar = QProgressBar(); self.energy_bar.setRange(0, 1000); self.energy_bar.setTextVisible(False)
        self.energy_bar.setFixedHeight(10)
        self.energy_bar.setStyleSheet("QProgressBar { border: 1px solid #333; background: #0e0e0e; }"
                                      f"QProgressBar::chunk {{ background-color: {GREEN}; }}")
        self.energy_lbl = QLabel("— Wh"); self.energy_lbl.setStyleSheet(MONO_CSS)
        elabel = QLabel("energy"); elabel.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        row = QWidget(); rl = QHBoxLayout(row); rl.setContentsMargins(0, 0, 0, 0); rl.setSpacing(6)
        rl.addWidget(elabel); rl.addWidget(self.energy_bar, 1); rl.addWidget(self.energy_lbl)
        lay.addWidget(row)
        self.i_active = Indicator("Active heaters", value="—")
        self.i_inhibit = Indicator("Motion inhibit", value="—")
        self.i_pid = Indicator("PID gains (ALL)", value="0.2 · 0.02 · 0.03")
        for ind in (self.i_active, self.i_inhibit, self.i_pid):
            lay.addWidget(ind)
        outer.addWidget(frame)

        frame, lay = group_box("Heaters")
        header = QHBoxLayout(); header.setSpacing(4)
        for text, width in (("", 22), ("meas. °C", 64), ("target", 62 + 32 + 32 + 8), ("duty", 0), ("state", 40)):
            lbl = QLabel(text); lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
            if width:
                lbl.setFixedWidth(width)
                header.addWidget(lbl)
            else:
                header.addWidget(lbl, 1)
        lay.addLayout(header)
        self.rows: List[HeaterRow] = []
        for i in range(HEATER_COUNT):
            row = HeaterRow(i, self)
            self.rows.append(row)
            lay.addWidget(row)
        self.resp_heaters = ResponseLine()
        lay.addWidget(self.resp_heaters)
        outer.addWidget(frame)

        frame, lay = group_box("All channels")
        self.all_target = QDoubleSpinBox()
        self.all_target.setRange(0.0, 80.0); self.all_target.setDecimals(1); self.all_target.setValue(DEFAULT_TARGET_C)
        self.all_target.setSuffix(" °C")
        self.btn_set_all = make_button("Set all", "success", sends="SET_ALL_TEMP_TARGETS <target_c>", min_height=24,
                                       slot=self.set_all_targets)
        self.btn_clear_all = make_button("Clear all", "neutral", sends="CLEAR_TEMP_TARGETS", min_height=24,
                                         slot=lambda: self._send("CLEAR_TEMP_TARGETS"))
        self.btn_refresh = make_button("Refresh", "neutral", sends="GET_THERMAL", min_height=24,
                                       slot=lambda: self._send("GET_THERMAL"))
        lay.addWidget(hrow(self.all_target, self.btn_set_all, self.btn_clear_all, self.btn_refresh))
        self.resp_all = ResponseLine()
        lay.addWidget(self.resp_all)
        outer.addWidget(frame)

        frame, lay = group_box("Presets")
        self.preset_select = QComboBox()
        self.btn_apply_preset = make_button("Apply", "success", min_height=24, slot=self.apply_preset)
        self.btn_apply_preset.setToolTip("Sends: SET_PID ALL …, then SET_TEMP_TARGET / CLEAR_TEMP_TARGET per heater")
        self.btn_save_preset = make_button("Save as…", "neutral", min_height=24, slot=self.save_preset)
        self.btn_save_preset.setToolTip("Saves the six target spin-boxes and the PID gains locally — no wire command.")
        self.btn_capture = make_button("Capture", "neutral", min_height=24, slot=self.capture_preset)
        self.btn_capture.setToolTip("Sends: GET_THERMAL, then saves the onboard's current targets as a preset.")
        lay.addWidget(hrow(self.preset_select, self.btn_apply_preset, self.btn_save_preset, self.btn_capture))
        self.preset_info = QLabel(str(self.presets.path))
        self.preset_info.setWordWrap(True); self.preset_info.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.preset_info)
        self.resp_preset = ResponseLine()
        lay.addWidget(self.resp_preset)
        outer.addWidget(frame)
        outer.addStretch()

        self._capture_pending: Optional[str] = None
        self.refresh_presets()
        self.update_state(self.state)

    # -- senders -----------------------------------------------------------------
    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def set_target(self, index: int, value: float) -> None:
        ok, norm = validate_temperature_target(value, self._limits[0], self._limits[1])
        if not ok:
            self.resp_heaters.show_note(f"✖ H{index}: {norm}", RED)
            return
        self._send(f"SET_TEMP_TARGET {index} {norm}")

    def clear_target(self, index: int) -> None:
        self._send(f"CLEAR_TEMP_TARGET {index}")

    def set_all_targets(self) -> None:
        ok, norm = validate_temperature_target(self.all_target.value(), self._limits[0], self._limits[1])
        if not ok:
            self.resp_all.show_note(f"✖ {norm}", RED)
            return
        for row in self.rows:
            row.target.setValue(self.all_target.value())
        self._send(f"SET_ALL_TEMP_TARGETS {norm}")

    # -- presets -----------------------------------------------------------------
    def refresh_presets(self) -> None:
        current = self.preset_select.currentText()
        self.preset_select.clear()
        self.preset_select.addItems(self.presets.names())
        if current in self.presets.names():
            self.preset_select.setCurrentText(current)
        if self.presets.load_error:
            self.preset_info.setText(f"preset file unreadable: {self.presets.load_error}")
        elif self.presets.migrated_from:
            self.preset_info.setText(f"migrated from {self.presets.migrated_from.name} · {self.presets.path}")
        else:
            self.preset_info.setText(str(self.presets.path))

    def current_preset_from_ui(self, name: str) -> ThermalPreset:
        return ThermalPreset(name, targets_c=[row.target.value() for row in self.rows], pid_all=self._gains)

    def save_preset(self) -> None:
        name, ok = QInputDialog.getText(self, "Save preset", "Preset name:")
        name = (name or "").strip()
        if not ok or not name:
            return
        self.presets.put(self.current_preset_from_ui(name))
        self.refresh_presets()
        self.preset_select.setCurrentText(name)
        self.resp_preset.show_note(f"✔ saved preset '{name}' to {self.presets.path}", GREEN)

    def apply_preset(self) -> None:
        name = self.preset_select.currentText()
        preset = self.presets.get(name)
        if preset is None:
            self.resp_preset.show_note("✖ select a preset", RED)
            return
        for index, target in enumerate(preset.targets_c[:HEATER_COUNT]):
            if target is not None:
                self.rows[index].target.setValue(target)
        for cmd in preset.commands():
            self._send(cmd)
        self.presets.mark_applied(name)
        self.resp_preset.show_note(f"✔ applying '{name}': {len(preset.commands())} commands sent", GREEN)

    def capture_preset(self) -> None:
        name, ok = QInputDialog.getText(self, "Capture preset", "Preset name for the onboard's current targets:")
        name = (name or "").strip()
        if not ok or not name:
            return
        self._capture_pending = name
        self._send("GET_THERMAL")

    # -- inputs ------------------------------------------------------------------
    def set_gains(self, kp: float, ki: float, kd: float) -> None:
        self._gains = (kp, ki, kd)
        self.i_pid.set_value(f"{kp:g} · {ki:g} · {kd:g}")

    def _targets_updated(self) -> None:
        self.targets_changed.emit(list(self._targets))
        self.update_state(self.state)

    def update_state(self, state: OnboardState) -> None:
        self.state = state
        inhibited = state.heaters_inhibited
        for row in self.rows:
            temp = state.sample_temps[HEATER_SAMPLE[row.index]] if state.have_packet else None
            duty = state.heater_duty[row.index] if state.have_packet else 0.0
            row.update_row(temp, duty, self._targets[row.index], inhibited, gating.heater_reason(state, row.index))
        self.btn_set_all.set_reason(gating.all_heaters_reason(state))
        for btn in (self.btn_clear_all, self.btn_refresh, self.btn_apply_preset, self.btn_capture):
            btn.set_reason(gating.generic_reason(state))
        if state.have_packet:
            if state.energy_wh is not None and state.budget_wh:
                frac = max(0.0, min(1.0, state.energy_wh / state.budget_wh))
                self.energy_bar.setValue(int(frac * 1000))
                color = GREEN if frac < 0.7 else AMBER if frac < 0.9 else RED
                self.energy_bar.setStyleSheet("QProgressBar { border: 1px solid #333; background: #0e0e0e; }"
                                              f"QProgressBar::chunk {{ background-color: {color}; }}")
                self.energy_lbl.setText(f"{state.energy_wh:.1f} / {state.budget_wh:.0f} Wh")
            elif state.energy_wh is None:
                self.energy_lbl.setText("not reported")
            active = sum(1 for d in state.heater_duty if d > 0.0)
            reported = f"{state.heaters_active}" if state.heaters_active is not None else f"{active}"
            self.i_active.set_value(f"{reported} / 3 max", AMBER if active >= 3 else GREEN)
            self.i_active.set_color(AMBER if active >= 3 else GREEN)
            moving = [f"M{m.motor_id}" for m in state.motors if m.moving or m.holding]
            self.i_inhibit.set_value(("ACTIVE " + " ".join(moving)).strip() if inhibited else "none",
                                     AMBER if inhibited else GREEN)
            self.i_inhibit.set_color(AMBER if inhibited else GREEN)

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb == "GET_THERMAL" and resp.ok:
            self._absorb_get_thermal(resp.body)
        if tag is not self:
            return
        if verb in ("SET_TEMP_TARGET", "CLEAR_TEMP_TARGET"):
            self.resp_heaters.show_response(cmd, resp, ms)
            if resp.ok:
                parts = cmd.split()
                try:
                    index = int(parts[1])
                except (IndexError, ValueError):
                    return
                self._targets[index] = float(parts[2]) if verb == "SET_TEMP_TARGET" and len(parts) > 2 else None
                self._targets_updated()
        elif verb in ("SET_ALL_TEMP_TARGETS", "CLEAR_TEMP_TARGETS", "GET_THERMAL"):
            self.resp_all.show_response(cmd, resp, ms)
            if resp.ok and verb == "SET_ALL_TEMP_TARGETS":
                self._targets = [float(cmd.split()[1])] * HEATER_COUNT
                self._targets_updated()
            elif resp.ok and verb == "CLEAR_TEMP_TARGETS":
                self._targets = [None] * HEATER_COUNT
                self._targets_updated()
        elif verb == "SET_PID":
            self.resp_preset.show_response(cmd, resp, ms)

    def _absorb_get_thermal(self, body: str) -> None:
        kv = parse_kv_body(body)
        try:
            lo = float(kv.get("target_min_c", self._limits[0]))
            hi = float(kv.get("target_max_c", self._limits[1]))
            self._limits = (lo, hi)
            for row in self.rows:
                row.target.setRange(lo, hi)
            self.all_target.setRange(lo, hi)
        except ValueError:
            pass
        for index in range(HEATER_COUNT):
            raw = kv.get(f"h{index}_target", "-")
            try:
                self._targets[index] = None if raw in ("-", "") else float(raw)
            except ValueError:
                self._targets[index] = None
            if self._targets[index] is not None:
                self.rows[index].target.setValue(self._targets[index])
        if self._capture_pending:
            name = self._capture_pending
            self._capture_pending = None
            self.presets.put(ThermalPreset.from_get_thermal(name, body, self._gains))
            self.refresh_presets()
            self.preset_select.setCurrentText(name)
            self.resp_preset.show_note(f"✔ captured onboard targets as preset '{name}'", GREEN)
        self._targets_updated()

    # -- test accessors ----------------------------------------------------------
    def targets(self) -> List[Optional[float]]:
        return list(self._targets)
