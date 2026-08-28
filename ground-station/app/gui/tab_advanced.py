"""Advanced tab: bend sequences, PID tuning, open-loop duty, microstep,
preset management, network, fallback plan (redesign spec §5.5)."""
from __future__ import annotations

from typing import List, Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QComboBox, QDoubleSpinBox, QFileDialog, QGridLayout, QHBoxLayout, QInputDialog,
    QLabel, QLineEdit, QScrollArea, QSlider, QSpinBox, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget,
)

from ..protocol import (
    CommandResponse, validate_duty, validate_microstep, validate_pid_gains, validate_speed_hz,
    validate_stepper_move,
)
from ..thermal_presets import PresetStore
from . import gating
from .dispatch import CommandDispatcher
from .state import HEATER_COUNT, MOTOR_COUNT, OnboardState
from .theme import HEATER_COLORS
from .widgets import (
    AMBER, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, confirm, group_box, hrow, make_button,
)


class AdvancedTab(QScrollArea):
    gains_changed = pyqtSignal(float, float, float)
    priority_changed = pyqtSignal(int)
    host_override_changed = pyqtSignal(str)

    def __init__(self, dispatcher: CommandDispatcher, preset_store: PresetStore, *,
                 bind: str = "0.0.0.0", tel_port: int = 4000, cmd_port: int = 5000,
                 discovery_port: int = 4100, priority: int = 100, host_override: str = "", parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self.presets = preset_store
        self.state = OnboardState()
        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        # -- Bend sequences ----------------------------------------------------
        frame, lay = group_box("Bend sequences")
        self.seq_motor = Segmented([("M0", 0), ("M1", 1)], current=0)
        self.seq_name = QLineEdit(); self.seq_name.setPlaceholderText("sequence name (one word)")
        self.btn_seq_add = make_button("+", "neutral", min_height=22, slot=self._add_step)
        self.btn_seq_del = make_button("−", "neutral", min_height=22, slot=self._remove_step)
        lay.addWidget(hrow(self.seq_motor, self.seq_name, self.btn_seq_add, self.btn_seq_del))
        self.seq_table = QTableWidget(0, 3)
        self.seq_table.setHorizontalHeaderLabels(["target µst", "hold s", "speed Hz"])
        self.seq_table.verticalHeader().setVisible(False)
        self.seq_table.setMinimumHeight(110)
        self.seq_table.horizontalHeader().setStretchLastSection(True)
        lay.addWidget(self.seq_table)
        self._add_step()
        grid = QGridLayout(); grid.setSpacing(3)
        self.btn_seq_load = make_button("LOAD", "primary", sends="BENDSEQ_LOAD <motor_id> <name> <target>:<hold>[:<hz>] ...", min_height=24, slot=self._seq_load)
        self.btn_seq_run = make_button("RUN", "success", sends="BENDSEQ_RUN <motor_id> <name>", min_height=24, slot=self._seq_run)
        self.btn_seq_pause = make_button("PAUSE", "danger", sends="BENDSEQ_PAUSE <motor_id>", min_height=24, slot=lambda: self._seq_cmd("BENDSEQ_PAUSE"))
        self.btn_seq_resume = make_button("RESUME", "success", sends="BENDSEQ_RESUME <motor_id>", min_height=24, slot=lambda: self._seq_cmd("BENDSEQ_RESUME"))
        self.btn_seq_stop = make_button("STOP", "danger", sends="BENDSEQ_STOP <motor_id>", min_height=24, slot=lambda: self._seq_cmd("BENDSEQ_STOP"))
        self.btn_seq_status = make_button("STATUS", "neutral", sends="BENDSEQ_STATUS <motor_id>", min_height=24, slot=lambda: self._seq_cmd("BENDSEQ_STATUS"))
        self.btn_seq_clear = make_button("CLEAR", "neutral", sends="BENDSEQ_CLEAR <motor_id> [name]", min_height=24, slot=self._seq_clear)
        for i, btn in enumerate((self.btn_seq_load, self.btn_seq_run, self.btn_seq_pause, self.btn_seq_resume,
                                 self.btn_seq_stop, self.btn_seq_status, self.btn_seq_clear)):
            grid.addWidget(btn, i // 4, i % 4)
        self.seq_table.setMinimumWidth(1)
        lay.addLayout(grid)
        self.resp_seq = ResponseLine()
        lay.addWidget(self.resp_seq)
        outer.addWidget(frame)

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
        note = QLabel("Changing the divisor rescales positions — re-zero the motor afterwards. Commissioning default 4.")
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
        lay.addWidget(hrow(self.preset_select, self.btn_preset_rename, self.btn_preset_delete, self.btn_preset_export))
        self.preset_note = QLabel(""); self.preset_note.setWordWrap(True); self.preset_note.setMinimumWidth(1)
        self.preset_note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.preset_note)
        outer.addWidget(frame)

        # -- Fallback plan (Phase C) -------------------------------------------
        frame, lay = group_box("Fallback plan (onboard, link-loss only)")
        self.plan_rows = []
        for motor_id in range(MOTOR_COUNT):
            name = QLabel(f"M{motor_id}"); name.setStyleSheet(f"{MONO_CSS} font-weight: bold;")
            target = QSpinBox(); target.setRange(-200000, 200000); target.setValue(800); target.setSuffix(" µst")
            hold = QDoubleSpinBox(); hold.setRange(0.0, 3600.0); hold.setDecimals(1); hold.setValue(5.0); hold.setSuffix(" s")
            speed = QSpinBox(); speed.setRange(1, 100); speed.setValue(100); speed.setSuffix(" Hz")
            for spin, width in ((target, 84), (hold, 60), (speed, 64)):
                spin.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
                spin.setFixedWidth(width)
            btn = make_button("LOAD", "primary", sends=f"FALLBACK_PLAN {motor_id} <target> <hold> <hz>", min_height=22,
                              compact=True, slot=lambda m=motor_id: self._plan_load(m))
            self.plan_rows.append((target, hold, speed, btn))
            lay.addWidget(hrow(name, target, hold, speed, btn))
        self.btn_plan_arm = make_button("ARM PLAN", "success", sends="FALLBACK_ARM", min_height=24, slot=self._plan_arm)
        self.btn_plan_disarm = make_button("DISARM", "danger", sends="FALLBACK_DISARM", min_height=24, slot=lambda: self._send("FALLBACK_DISARM"))
        self.btn_plan_status = make_button("STATUS", "neutral", sends="FALLBACK_STATUS", min_height=24, slot=lambda: self._send("FALLBACK_STATUS"))
        self.plan_state = QLabel("plan: —"); self.plan_state.setStyleSheet(f"{MONO_CSS} color: {MUTED};")
        lay.addWidget(hrow(self.btn_plan_arm, self.btn_plan_disarm, self.btn_plan_status, self.plan_state, stretch_last=True))
        note = QLabel("Trigger: fallback active, phase PRE_FLOAT/FLOAT, plan armed and not yet executed, motor enabled+zeroed+healthy, "
                      "sample temperature inside the configured window (or the deadline passed). Runs M0 then M1, emits EVT,PULL, never repeats.")
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
        self.host_override = QLineEdit(host_override); self.host_override.setPlaceholderText("auto (discovery / probe)")
        self.btn_host = make_button("Apply", "neutral", min_height=22, slot=lambda: self.host_override_changed.emit(self.host_override.text().strip()))
        hl = QLabel("onboard host override"); hl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(pl, self.priority, stretch_last=True))
        lay.addWidget(hrow(hl, self.host_override, self.btn_host))
        outer.addWidget(frame)

        bench = QLabel("Bench-only commands (ARM_DEBUG <token>, DISARM_DEBUG, HEATER_TEST, SET_BENCH_MODE) are console-only.")
        bench.setWordWrap(True); bench.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        outer.addWidget(bench)
        outer.addStretch()
        self.refresh_presets()
        self.update_state(self.state)

    # -- senders -------------------------------------------------------------------
    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def _seq_motor_id(self) -> int:
        return int(self.seq_motor.value() or 0)

    def _add_step(self) -> None:
        row = self.seq_table.rowCount()
        self.seq_table.insertRow(row)
        for col, text in enumerate(("0", "0", "")):
            self.seq_table.setItem(row, col, QTableWidgetItem(text))

    def _remove_step(self) -> None:
        row = self.seq_table.currentRow()
        if row < 0:
            row = self.seq_table.rowCount() - 1
        if row >= 0:
            self.seq_table.removeRow(row)

    def _seq_identity(self) -> Optional[str]:
        name = self.seq_name.text().strip()
        if not name or any(ch.isspace() for ch in name):
            self.resp_seq.show_note("✖ sequence name must be one word", RED)
            return None
        return name

    def encode_steps(self) -> Optional[List[str]]:
        encoded: List[str] = []
        for row in range(self.seq_table.rowCount()):
            try:
                target = int(self.seq_table.item(row, 0).text().strip())
                hold = float(self.seq_table.item(row, 1).text().strip())
                speed_text = self.seq_table.item(row, 2).text().strip()
            except (AttributeError, ValueError) as exc:
                self.resp_seq.show_note(f"✖ step {row + 1}: {exc}", RED)
                return None
            ok, msg = validate_stepper_move(target)
            if not ok:
                self.resp_seq.show_note(f"✖ step {row + 1}: {msg}", RED); return None
            if hold < 0:
                self.resp_seq.show_note(f"✖ step {row + 1}: hold must be non-negative", RED); return None
            step = f"{target}:{hold:g}"
            if speed_text:
                ok, msg = validate_speed_hz(float(speed_text))
                if not ok:
                    self.resp_seq.show_note(f"✖ step {row + 1}: {msg}", RED); return None
                step += f":{float(speed_text):g}"
            encoded.append(step)
        if not encoded:
            self.resp_seq.show_note("✖ add at least one step", RED)
            return None
        return encoded

    def _seq_load(self) -> None:
        name = self._seq_identity()
        steps = self.encode_steps() if name else None
        if name and steps:
            self._send(f"BENDSEQ_LOAD {self._seq_motor_id()} {name} {' '.join(steps)}")

    def _seq_run(self) -> None:
        name = self._seq_identity()
        if name and confirm(self, "Run bend sequence?", f"Send BENDSEQ_RUN {self._seq_motor_id()} {name}?"):
            self._send(f"BENDSEQ_RUN {self._seq_motor_id()} {name}")

    def _seq_cmd(self, verb: str) -> None:
        self._send(f"{verb} {self._seq_motor_id()}")

    def _seq_clear(self) -> None:
        name = self.seq_name.text().strip()
        self._send(f"BENDSEQ_CLEAR {self._seq_motor_id()}" + (f" {name}" if name else ""))

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

    def _plan_load(self, motor_id: int) -> None:
        target, hold, speed, _btn = self.plan_rows[motor_id]
        self._send(f"FALLBACK_PLAN {motor_id} {target.value()} {hold.value():g} {speed.value()}")

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
        motor_id = self._seq_motor_id()
        generic = gating.generic_reason(state)
        self.btn_seq_load.set_reason(generic)
        self.btn_seq_run.set_reason(gating.sequence_run_reason(state, motor_id))
        for btn in (self.btn_seq_pause, self.btn_seq_resume, self.btn_seq_stop, self.btn_seq_status, self.btn_seq_clear,
                    self.btn_pid, self.btn_clear_overrides, self.btn_us, self.btn_plan_arm, self.btn_plan_disarm,
                    self.btn_plan_status):
            btn.set_reason(generic)
        for _t, _h, _s, btn in self.plan_rows:
            btn.set_reason(generic)
        for i, btn in enumerate(self.duty_buttons):
            btn.set_reason(gating.heater_reason(state, i))
        for btn in self.all_duty_buttons:
            btn.set_reason(gating.all_heaters_reason(state))
        if state.plan_state is not None:
            self.plan_state.setText(f"plan: {state.plan_state}")
            color = {"armed": GREEN, "running": AMBER, "done": GREEN, "failed": RED}.get(state.plan_state, MUTED)
            self.plan_state.setStyleSheet(f"{MONO_CSS} color: {color};")

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb.startswith("BENDSEQ"):
            self.resp_seq.show_response(cmd, resp, ms)
        elif verb == "SET_PID":
            self.resp_pid.show_response(cmd, resp, ms)
            if resp.ok and cmd.split()[1].upper() == "ALL":
                self.gains_changed.emit(self.kp.value(), self.ki.value(), self.kd.value())
        elif verb in ("SET_HEATER_DUTY", "SET_ALL_DUTY", "CLEAR_OVERRIDES"):
            self.resp_duty.show_response(cmd, resp, ms)
        elif verb == "STEPPER_SET_MICROSTEP":
            self.resp_us.show_response(cmd, resp, ms)
        elif verb.startswith("FALLBACK"):
            self.resp_plan.show_response(cmd, resp, ms)
