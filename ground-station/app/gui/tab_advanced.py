"""Advanced tab: PID tuning, open-loop duty, microstep, preset management,
fallback plan, beacon priority (redesign spec §5.5). The bend cycle lives
on the Motion tab (2026-10-10) and the onboard IP on the System tab."""
from __future__ import annotations

from typing import List

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QComboBox, QDoubleSpinBox, QFileDialog, QHBoxLayout, QInputDialog,
    QLabel, QScrollArea, QSlider, QSpinBox, QVBoxLayout, QWidget,
)

from ..protocol import (
    CommandResponse, validate_duty, validate_microstep, validate_pid_gains, FULL_STEPS_PER_MM,
)
from ..thermal_presets import PresetStore
from . import gating
from .dispatch import CommandDispatcher
from .state import HEATER_COUNT, MOTOR_COUNT, OnboardState
from .theme import HEATER_COLORS
from .widgets import (
    AMBER, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, confirm, group_box, hrow, make_button,
    with_unit,
)


def _mm_to_usteps(mm: float, microstep: int) -> int:
    """mm of linear travel -> microsteps at the ball-screw lead
    (protocol.LEAD_MM_PER_REV, 1 mm: 2.0 mm is 400 full steps)."""
    return int(round(mm * FULL_STEPS_PER_MM * max(1, microstep)))


class AdvancedTab(QScrollArea):
    gains_changed = pyqtSignal(float, float, float)
    priority_changed = pyqtSignal(int)

    def __init__(self, dispatcher: CommandDispatcher, preset_store: PresetStore, *,
                 bind: str = "0.0.0.0", tel_port: int = 4000, cmd_port: int = 5000,
                 discovery_port: int = 4100, priority: int = 100, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self.presets = preset_store
        self.state = OnboardState()
        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        # -- PID tuning ------------------------------------------------------
        frame, lay = group_box("PID tuning")
        self.pid_channel = QComboBox()
        self.pid_channel.addItem("ALL", "ALL")
        for i in range(HEATER_COUNT):
            self.pid_channel.addItem(f"H{i}", str(i))
        self.kp = QDoubleSpinBox(); self.ki = QDoubleSpinBox(); self.kd = QDoubleSpinBox()
        for spin, value in ((self.kp, 0.20), (self.ki, 0.02), (self.kd, 0.03)):
            spin.setRange(0.0, 1000.0); spin.setDecimals(4); spin.setValue(value); spin.setMinimumWidth(70)
        self.btn_pid = make_button("Apply", "primary", sends="SET_PID <channel|ALL> <kp> <ki> <kd>", min_height=24, slot=self._apply_pid)
        kpl, kil, kdl = QLabel("Kp"), QLabel("Ki"), QLabel("Kd")
        for l in (kpl, kil, kdl):
            l.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        chl = QLabel("channel"); chl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(chl, self.pid_channel, self.btn_pid, stretch_last=True))
        lay.addWidget(hrow(kpl, self.kp, kil, self.ki, kdl, self.kd))
        self.resp_pid = ResponseLine()
        lay.addWidget(self.resp_pid)
        outer.addWidget(frame)

        # -- Open-loop duty --------------------------------------------------
        frame, lay = group_box("Open-loop duty")
        note = QLabel("Sets a fixed duty and clears that channel's target. Refused unless the channel's PT100 is valid "
                      "(or the bench debug arm is active). Never more than 3 heaters run at once (scheduler).")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.debug_note = QLabel(""); self.debug_note.setWordWrap(True); self.debug_note.setMinimumWidth(1)
        lay.addWidget(self.debug_note)
        self.duty_sliders: List[QSlider] = []
        self.duty_labels: List[QLabel] = []
        self.duty_buttons = []
        for i in range(HEATER_COUNT):
            name = QLabel(f"H{i}"); name.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {HEATER_COLORS[i]};"); name.setMinimumWidth(22)
            slider = QSlider(Qt.Orientation.Horizontal); slider.setRange(0, 100); slider.setSingleStep(5)
            value = QLabel("0 %"); value.setStyleSheet(MONO_CSS); value.setMinimumWidth(36)
            slider.valueChanged.connect(lambda v, lbl=value: lbl.setText(f"{v} %"))
            btn = make_button("Set", "primary", sends=f"SET_HEATER_DUTY {i} <duty>", min_height=20,
                              slot=lambda idx=i: self._set_duty(idx))
            self.duty_sliders.append(slider); self.duty_labels.append(value); self.duty_buttons.append(btn)
            lay.addWidget(hrow(name, slider, value, btn))
        presets = QHBoxLayout(); presets.setSpacing(3)
        self.all_duty_buttons = []
        for pct in (0, 25, 50):
            btn = make_button(f"All {pct} %", "neutral", sends=f"SET_ALL_DUTY {pct / 100:.3f}", min_height=22,
                              compact=True, slot=lambda p=pct: self._set_all_duty(p / 100.0))
            self.all_duty_buttons.append(btn); presets.addWidget(btn)
        lay.addLayout(presets)
        self.btn_clear_overrides = make_button("CLEAR_OVERRIDES", "neutral", sends="CLEAR_OVERRIDES", min_height=22,
                                               slot=lambda: self._send("CLEAR_OVERRIDES"))
        lay.addWidget(self.btn_clear_overrides)
        self.resp_duty = ResponseLine()
        lay.addWidget(self.resp_duty)
        outer.addWidget(frame)

        # -- Microstep -------------------------------------------------------
        frame, lay = group_box("Microstep")
        self.us_motor = Segmented([("M0", 0), ("M1", 1)], current=0)
        self.us = QComboBox(); self.us.addItems(["1", "2", "4", "8", "16", "32", "64", "128", "256"]); self.us.setCurrentText("4")
        self.btn_us = make_button("Set", "primary", sends="STEPPER_SET_MICROSTEP <motor_id> <n>", min_height=24, slot=self._set_microstep)
        lay.addWidget(hrow(self.us_motor, self.us, self.btn_us, stretch_last=True))
        note = QLabel("The firmware rescales its stored position so mm travel is preserved across a divisor change — no re-zero "
                      "needed. A memorised bend cycle and the fallback plan stay in raw µsteps onboard: MEMORISE / LOAD them "
                      "again after changing it. Commissioning default 4.")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.resp_us = ResponseLine()
        lay.addWidget(self.resp_us)
        outer.addWidget(frame)

        # -- Preset management -------------------------------------------------
        frame, lay = group_box("Preset management")
        self.preset_select = QComboBox()
        self.btn_preset_rename = make_button("Rename", "neutral", min_height=22, slot=self._rename_preset)
        self.btn_preset_delete = make_button("Delete", "danger", min_height=22, slot=self._delete_preset)
        self.btn_preset_export = make_button("Export…", "neutral", min_height=22, slot=self._export_presets)
        lay.addWidget(hrow(self.preset_select, self.btn_preset_rename, stretch_last=True))
        lay.addWidget(hrow(self.btn_preset_delete, self.btn_preset_export))
        self.preset_note = QLabel(""); self.preset_note.setWordWrap(True); self.preset_note.setMinimumWidth(1)
        self.preset_note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.preset_note)
        outer.addWidget(frame)

        # -- Fallback plan (Phase C) -------------------------------------------
        frame, lay = group_box("Fallback plan (onboard, link-loss only)")
        self.plan_rows = []
        for motor_id in range(MOTOR_COUNT):
            name = QLabel(f"M{motor_id}"); name.setStyleSheet(f"{MONO_CSS} font-weight: bold;")
            target = QDoubleSpinBox(); target.setRange(-500.0, 500.0); target.setDecimals(2)
            target.setValue(2.0)
            hold = QDoubleSpinBox(); hold.setRange(0.0, 3600.0); hold.setDecimals(1); hold.setValue(5.0)
            for spin, width in ((target, 62), (hold, 48)):
                spin.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
                spin.setFixedWidth(width)
            btn = make_button("LOAD", "primary", sends=f"FALLBACK_PLAN {motor_id} <target µst from mm> <hold>", min_height=22,
                              compact=True, slot=lambda m=motor_id: self._plan_load(m))
            self.plan_rows.append((target, hold, btn))
            lay.addWidget(hrow(name, with_unit(target, "mm"), with_unit(hold, "s"), btn, stretch_last=True))
        self.btn_plan_arm = make_button("ARM PLAN", "success", sends="FALLBACK_ARM", min_height=24, slot=self._plan_arm)
        self.btn_plan_disarm = make_button("DISARM", "danger", sends="FALLBACK_DISARM", min_height=24, slot=lambda: self._send("FALLBACK_DISARM"))
        self.btn_plan_status = make_button("STATUS", "neutral", sends="FALLBACK_STATUS", min_height=24, slot=lambda: self._send("FALLBACK_STATUS"))
        self.plan_state = QLabel("plan: —"); self.plan_state.setStyleSheet(f"{MONO_CSS} color: {MUTED};")
        lay.addWidget(hrow(self.btn_plan_arm, self.btn_plan_disarm, self.btn_plan_status))
        lay.addWidget(self.plan_state)
        note = QLabel("Trigger: fallback active, phase PRE_FLOAT/FLOAT, plan armed and not yet executed, motor enabled+zeroed+healthy, "
                      "sample temperature inside the configured window (or the deadline passed). Runs M0 then M1 at each motor's "
                      "own speed and acceleration (Motion tab, Drive settings), emits EVT,PULL, never repeats.")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.resp_plan = ResponseLine()
        lay.addWidget(self.resp_plan)
        outer.addWidget(frame)

        # -- Network -----------------------------------------------------------
        frame, lay = group_box("Network")
        info = QLabel(f"bind {bind} · telemetry {tel_port} · command {cmd_port} · discovery {discovery_port}")
        info.setStyleSheet(f"{MONO_CSS} color: {MUTED};"); info.setWordWrap(True)
        lay.addWidget(info)
        self.priority = QSpinBox(); self.priority.setRange(0, 999); self.priority.setValue(priority)
        self.priority.setToolTip("GS beacon priority — higher wins; a backup ground station should be lower (e.g. 50).")
        self.priority.valueChanged.connect(self.priority_changed.emit)
        pl = QLabel("beacon priority"); pl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(pl, self.priority, stretch_last=True))
        note = QLabel("The onboard IP is set on the System tab (Link); the beacon stays the fallback when it is left on AUTO.")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        outer.addWidget(frame)

        bench = QLabel("Bench-only commands (ARM_DEBUG <token>, DISARM_DEBUG, HEATER_TEST, SET_BENCH_MODE) and "
                       "STEPLOSS_ACK <motor> are console-only.")
        bench.setWordWrap(True); bench.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        outer.addWidget(bench)
        outer.addStretch()
        self.refresh_presets()
        self.update_state(self.state)

    # -- senders -------------------------------------------------------------------
    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def _apply_pid(self) -> None:
        ok, gains = validate_pid_gains(self.kp.value(), self.ki.value(), self.kd.value())
        if not ok:
            self.resp_pid.show_note(f"✖ {gains}", RED); return
        self._send(f"SET_PID {self.pid_channel.currentData()} {gains}")

    def _set_duty(self, index: int) -> None:
        ok, d = validate_duty(self.duty_sliders[index].value() / 100.0)
        if not ok:
            self.resp_duty.show_note(f"✖ {d}", RED); return
        self._send(f"SET_HEATER_DUTY {index} {d}")

    def _set_all_duty(self, duty: float) -> None:
        ok, d = validate_duty(duty)
        if ok:
            self._send(f"SET_ALL_DUTY {d}")

    def _set_microstep(self) -> None:
        ok, norm = validate_microstep(int(self.us.currentText()))
        if not ok:
            self.resp_us.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_SET_MICROSTEP {int(self.us_motor.value() or 0)} {norm}")

    def _live_microstep(self, motor_id: int) -> int | None:
        """The motor's live divisor from telemetry, or None while the console
        has not seen a STEPPER<n> segment for it. Absolute µstep targets
        must never be encoded against a guessed divisor: the failsafe plan
        executes on its own, with nobody there to notice a 4x short bend."""
        motor = self.state.motor(motor_id)
        return motor.microstep if motor.present and motor.microstep > 0 else None

    def _plan_load(self, motor_id: int) -> None:
        target, hold, _btn = self.plan_rows[motor_id]
        us = self._live_microstep(motor_id)
        if us is None:
            self.resp_plan.show_note(
                f"✖ M{motor_id} microstep unknown — wait for telemetry before loading the plan", RED)
            return
        usteps = _mm_to_usteps(target.value(), us)
        # No speed field: the plan runs at the motor's own speed and
        # acceleration (STEPPER_SET_SPEED / STEPPER_SET_ACCEL).
        self._send(f"FALLBACK_PLAN {motor_id} {usteps} {hold.value():g}")

    def _plan_arm(self) -> None:
        if confirm(self, "Arm the fallback plan?", "Send FALLBACK_ARM? The onboard will execute the loaded plan on its own if the link is lost at PRE_FLOAT/FLOAT."):
            self._send("FALLBACK_ARM")

    # -- presets ---------------------------------------------------------------------
    def refresh_presets(self) -> None:
        current = self.preset_select.currentText()
        self.preset_select.clear()
        self.preset_select.addItems(self.presets.names())
        if current in self.presets.names():
            self.preset_select.setCurrentText(current)
        preset = self.presets.get(self.preset_select.currentText())
        if preset is not None:
            targets = " ".join("—" if t is None else f"{t:g}" for t in preset.targets_c)
            self.preset_note.setText(f"targets {targets} · PID {preset.pid_all[0]:g}/{preset.pid_all[1]:g}/{preset.pid_all[2]:g}"
                                     f" · created {preset.created_utc or '?'} · last applied {preset.last_applied_utc or 'never'}")
        else:
            self.preset_note.setText("no presets")

    def _rename_preset(self) -> None:
        old = self.preset_select.currentText()
        if not old:
            return
        new, ok = QInputDialog.getText(self, "Rename preset", "New name:", text=old)
        if ok and new.strip() and self.presets.rename(old, new.strip()):
            self.refresh_presets()

    def _delete_preset(self) -> None:
        name = self.preset_select.currentText()
        if name and confirm(self, "Delete preset?", f"Delete '{name}' from {self.presets.path}? The previous file is kept as .bak."):
            self.presets.delete(name)
            self.refresh_presets()

    def _export_presets(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Export presets", "thermal_presets.json", "JSON (*.json)")
        if path:
            try:
                data = self.presets.path.read_text(encoding="utf-8") if self.presets.path.exists() else "{}"
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(data)
                self.preset_note.setText(f"exported to {path}")
            except OSError as exc:
                self.preset_note.setText(f"export failed: {exc}")

    # -- inputs ------------------------------------------------------------------------
    def update_state(self, state: OnboardState) -> None:
        self.state = state
        generic = gating.generic_reason(state)
        for btn in (self.btn_pid, self.btn_clear_overrides, self.btn_us, self.btn_plan_arm, self.btn_plan_disarm,
                    self.btn_plan_status):
            btn.set_reason(generic)
        for _t, _h, btn in self.plan_rows:
            btn.set_reason(generic)
        for i, btn in enumerate(self.duty_buttons):
            btn.set_reason(gating.duty_reason(state, i))
        for btn in self.all_duty_buttons:
            btn.set_reason(gating.all_duty_reason(state))
        if state.debug_armed:
            self.debug_note.setText("DEBUG ARM ACTIVE — open-loop duty allowed without PT100 feedback (DISARM_DEBUG to end)")
            self.debug_note.setStyleSheet(f"color: {AMBER}; font-size: 8pt; font-weight: bold;")
        else:
            self.debug_note.setText("")
        if state.plan_state is not None:
            self.plan_state.setText(f"plan: {state.plan_state}")
            color = {"armed": GREEN, "running": AMBER, "done": GREEN, "failed": RED}.get(state.plan_state, MUTED)
            self.plan_state.setStyleSheet(f"{MONO_CSS} color: {color};")

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb == "SET_PID":
            self.resp_pid.show_response(cmd, resp, ms)
            if resp.ok and cmd.split()[1].upper() == "ALL":
                self.gains_changed.emit(self.kp.value(), self.ki.value(), self.kd.value())
        elif verb in ("SET_HEATER_DUTY", "SET_ALL_DUTY", "CLEAR_OVERRIDES"):
            self.resp_duty.show_response(cmd, resp, ms)
        elif verb == "STEPPER_SET_MICROSTEP":
            self.resp_us.show_response(cmd, resp, ms)
        elif verb.startswith("FALLBACK"):
            self.resp_plan.show_response(cmd, resp, ms)
