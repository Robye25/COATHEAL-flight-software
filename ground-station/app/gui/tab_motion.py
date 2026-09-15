"""Motion tab: the ascent bend (redesign spec §5.4).

Two always-visible motor cards, a selector that drives the shared
controls, jog in mm (STEPPER_MOVE_MM; the onboard converts through the
ball-screw lead), per-motor drive settings (speed, run current, accel),
BEND (STEPPER_MOVETO_MM with hold), STANDARD PULL (PULL_EXECUTE), and the
resistance-before/after readout that confirms a bend. Every control that
cannot succeed is disabled with its reason.
"""
from __future__ import annotations

from collections import deque
from typing import Deque, List, Optional

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QDoubleSpinBox, QFrame, QGridLayout, QHBoxLayout, QLabel, QScrollArea,
    QVBoxLayout, QWidget,
)

from ..protocol import (
    CommandResponse, PullEvent, validate_accel_mm_s2, validate_current_a,
    FULL_STEPS_PER_MM, LEAD_MM_PER_REV, MAX_ACCEL_MM_S2, MAX_SPEED_MM_S, mm_s_from_hz,
    validate_move_mm, validate_speed_mm_s,
)
from ..telemetry_log import utc_now_iso
from . import gating
from .bend_tracker import BendTracker
from .dispatch import CommandDispatcher
from .state import DEFAULT_LAYOUT, MOTOR_COUNT, Layout, MotorState, OnboardState
from .widgets import (
    AMBER, BLUE, GRAY, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, StatusDot,
    group_box, hrow, make_button, with_unit,
)

MOTOR_COLORS = ("#2ecc71", "#e67e22")
# Jog distances in mm (STEPPER_MOVE_MM; the onboard converts through
# stepper.lead_mm_per_rev). At the commissioning defaults (2 mm lead) the
# largest jog is 2.5 revolutions.
JOG_MM = (-0.1, -1.0, -5.0, 0.1, 1.0, 5.0)
DEFAULT_SPEED_MM_S = MAX_SPEED_MM_S   # the onboard ceiling (50 full-steps/s at the 2 mm lead)
DEFAULT_BEND_MM = 2.0   # one revolution at the 2 mm default lead
DEFAULT_HOLD_S = 5.0
DEFAULT_CURRENT_A = 0.8
DEFAULT_ACCEL_MM_S2 = 2.0             # 200 full-steps/s²


def motor_group_text(layout: Layout, motor_id: int) -> str:
    """`S0 S1 S2 S3 · H0–H3`-style summary of what a motor pulls."""
    samples = layout.motor_samples[motor_id] if motor_id < len(layout.motor_samples) else ()
    heaters = layout.heaters_of_motor(motor_id)
    return (" ".join(f"S{s}" for s in samples) + " · "
            + (" ".join(f"H{h}" for h in heaters) if heaters else "no heaters"))


class MotorCard(QFrame):
    def __init__(self, motor_id: int, parent=None):
        super().__init__(parent)
        self.motor_id = motor_id
        self.setObjectName("motorCard")
        self.setStyleSheet("QFrame#motorCard { background: #111; border: 1px solid #333; border-radius: 4px; }")
        lay = QVBoxLayout(self); lay.setContentsMargins(8, 6, 8, 6); lay.setSpacing(3)
        title = QLabel(f"M{motor_id}")
        title.setStyleSheet(f"font-weight: bold; font-size: 12pt; color: {MOTOR_COLORS[motor_id]}; border: none;")
        self.group = QLabel(motor_group_text(DEFAULT_LAYOUT, motor_id))
        self.group.setStyleSheet(f"color: {MUTED}; font-size: 9pt; border: none;")
        self.group.setWordWrap(True); self.group.setMinimumWidth(1)
        lay.addWidget(hrow(title, self.group, stretch_last=True))
        dots = QHBoxLayout(); dots.setSpacing(1)
        self.dots = {}
        tips = {"EN": "driver power stage enabled", "ZERO": "software zero set (SET_POSITION_ZERO)",
                "MOV": "pulses being issued", "HOLD": "at target, hold countdown running",
                "OK": "driver backend healthy", "DRV": "driver die thermal state (TMC5160 flags)"}
        for key in ("EN", "ZERO", "MOV", "HOLD", "OK", "DRV"):
            lbl = QLabel(key); lbl.setStyleSheet(f"color: {MUTED}; font-size: 7pt; border: none;")
            lbl.setToolTip(tips[key])
            dot = StatusDot(8); dot.set_color(GRAY)
            dot.setToolTip(tips[key])
            dots.addWidget(lbl); dots.addWidget(dot); dots.addSpacing(1)
            self.dots[key] = dot
        dots.addStretch()
        lay.addLayout(dots)
        # A shutdown/pre-warning banner: the 8 px DRV dot alone is too easy
        # to miss when the safety has just disabled the motor.
        self.thermal_note = QLabel(""); self.thermal_note.setWordWrap(True); self.thermal_note.setMinimumWidth(1)
        self.thermal_note.setStyleSheet(f"color: {RED}; font-size: 8pt; font-weight: bold; border: none;")
        self.thermal_note.hide()
        lay.addWidget(self.thermal_note)
        self.pos = self._kv(lay, "pos / tgt")
        # The operator's primary number during a bend — give it weight.
        self.pos.setStyleSheet(f"{MONO_CSS} border: none; font-size: 11pt; font-weight: bold;")
        self.speed = self._kv(lay, "drive")
        self.src = self._kv(lay, "last cmd")
        self.seq = self._kv(lay, "sequence")
        sep = QLabel("resistance"); sep.setStyleSheet(f"color: {MUTED}; font-size: 8pt; border: none; border-bottom: 1px solid #333;")
        lay.addWidget(sep)
        self.r_now = self._kv(lay, "R now")
        self.r_start = self._kv(lay, "bend start")
        self.r_delta = self._kv(lay, "Δ")

    def _kv(self, lay: QVBoxLayout, key: str) -> QLabel:
        row = QWidget(); h = QHBoxLayout(row); h.setContentsMargins(0, 0, 0, 0); h.setSpacing(6)
        k = QLabel(key); k.setStyleSheet(f"color: {MUTED}; font-size: 8pt; border: none;"); k.setFixedWidth(52)
        v = QLabel("—"); v.setStyleSheet(f"{MONO_CSS} border: none;"); v.setMinimumWidth(1)
        h.addWidget(k); h.addWidget(v, 1)
        lay.addWidget(row)
        return v

    def update_card(self, motor: MotorState, readout) -> None:
        if not motor.present:
            self.thermal_note.hide()
            for dot in self.dots.values():
                dot.set_color(GRAY)
            for lbl in (self.pos, self.speed, self.src, self.seq, self.r_now, self.r_start, self.r_delta):
                lbl.setText("—")
            return
        self.dots["EN"].set_color(GREEN if motor.enabled else "#555555")
        self.dots["ZERO"].set_color(GRAY if motor.zeroed is None else GREEN if motor.zeroed else RED)
        self.dots["ZERO"].setToolTip("zeroed: unknown (old firmware)" if motor.zeroed is None else
                                     "zeroed" if motor.zeroed else "not zeroed — SET ZERO at the reference position")
        self.dots["MOV"].set_color(AMBER if motor.moving else "#333333")
        self.dots["HOLD"].set_color(BLUE if motor.holding else "#333333")
        self.dots["OK"].set_color(GREEN if motor.healthy else RED)
        # TMC5160 die thermal state: threshold flags over SPI (the chip has
        # no numeric temperature ADC).
        drv = self.dots["DRV"]
        if motor.thermal == "hot":
            drv.set_color(RED)
            drv.setToolTip("driver ≥150 °C: over-temperature shutdown — motor disabled; cool, then ENABLE")
        elif motor.thermal == "warn":
            drv.set_color(AMBER)
            drv.setToolTip("driver ≥120 °C: pre-warning — reduce run current or duty")
        elif motor.thermal == "ok":
            drv.set_color(GREEN)
            drv.setToolTip("driver die < 120 °C")
        else:
            drv.set_color(GRAY)
            drv.setToolTip("driver thermal state unknown (old firmware)")
        if motor.thermal == "hot":
            self.thermal_note.setText("DRIVER ≥150 °C — motor disabled by safety; cool, then ENABLE")
            self.thermal_note.setStyleSheet(f"color: {RED}; font-size: 8pt; font-weight: bold; border: none;")
            self.thermal_note.show()
        elif motor.thermal == "warn":
            self.thermal_note.setText("driver ≥120 °C — reduce run current or duty")
            self.thermal_note.setStyleSheet(f"color: {AMBER}; font-size: 8pt; font-weight: bold; border: none;")
            self.thermal_note.show()
        else:
            self.thermal_note.hide()
        if motor.mm is not None and motor.mm_tgt is not None:
            pos_text = f"{motor.mm:.3f} / {motor.mm_tgt:.3f} mm"
            if motor.moving and motor.hz > 0:
                # ETA from |distance| / speed.
                mm_s = mm_s_from_hz(motor.hz)
                eta = abs(motor.mm_tgt - motor.mm) / mm_s if mm_s > 0 else 0.0
                pos_text += f" · ~{eta:.0f} s left"
            elif motor.holding and motor.hold_s > 0:
                pos_text += f" · hold {motor.hold_s:.0f} s left"
            self.pos.setText(pos_text)
            self.pos.setToolTip(f"{motor.position} / {motor.target} µst")
        else:
            self.pos.setText(f"{motor.position} / {motor.target} µst")
            self.pos.setToolTip("no mm telemetry (old firmware) — raw microsteps shown")
        speed = f"{mm_s_from_hz(motor.hz):.2f} mm/s · µ{motor.microstep}"
        if motor.amps is not None:
            speed += f" · {motor.amps:.2f} A"
        if motor.accel is not None:
            speed += f" · {motor.accel / FULL_STEPS_PER_MM:.2f} mm/s²"
        if motor.missed:
            speed += f" · missed {motor.missed}"
        self.speed.setText(speed)
        self.src.setText(motor.source or "—")
        if motor.seq_state in ("run", "pause"):
            self.seq.setText(f"{motor.seq_name or '?'} · {motor.seq_state}")
            self.seq.setStyleSheet(f"{MONO_CSS} border: none; color: {AMBER if motor.seq_state == 'pause' else GREEN};")
        else:
            self.seq.setText("idle" if motor.seq_state else "—")
            self.seq.setStyleSheet(f"{MONO_CSS} border: none; color: {MUTED};")
        if readout.sample is None:
            self.r_now.setText("no monitored specimen")
            self.r_start.setText("—"); self.r_delta.setText("—")
            return
        self.r_now.setText(f"S{readout.sample} {readout.r_now:.2f} Ω" if readout.r_now is not None else f"S{readout.sample} —")
        self.r_start.setText(f"{readout.r_start:.2f} Ω" if readout.r_start is not None else "—")
        if readout.delta_pct is None:
            self.r_delta.setText("—"); self.r_delta.setStyleSheet(f"{MONO_CSS} border: none; color: {MUTED};")
        else:
            self.r_delta.setText(f"{readout.delta_pct:+.2f} %")
            self.r_delta.setToolTip(f"since {readout.started_utc or '?'} ({readout.source} mark)")
            self.r_delta.setStyleSheet(f"{MONO_CSS} border: none; color: {GREEN if abs(readout.delta_pct) >= 0.5 else MUTED};")


class MotionTab(QScrollArea):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self.state = OnboardState()
        self.tracker = BendTracker()
        self._pulls: Deque[tuple] = deque(maxlen=3)  # (PullEvent, mm frozen at arrival)

        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        cards = QHBoxLayout(); cards.setSpacing(6)
        self.cards: List[MotorCard] = []
        for motor_id in range(MOTOR_COUNT):
            card = MotorCard(motor_id)
            self.cards.append(card)
            cards.addWidget(card, 1)
        outer.addLayout(cards)

        frame, lay = group_box("Selected motor")
        self.selector = Segmented([("M0", 0), ("M1", 1)], current=0)
        self.selector.valueChanged.connect(lambda _v: self.update_state(self.state))
        self.sel_status = QLabel("—"); self.sel_status.setStyleSheet(f"{MONO_CSS} color: {MUTED};")
        self.sel_status.setWordWrap(True)
        lay.addWidget(hrow(self.selector, self.sel_status))
        grid = QGridLayout(); grid.setSpacing(4)
        self.btn_enable = make_button("ENABLE", "success", sends="STEPPER_ENABLE <motor_id>", slot=lambda: self._send_motor("STEPPER_ENABLE"))
        self.btn_disable = make_button("DISABLE", "danger", sends="STEPPER_DISABLE <motor_id>", slot=lambda: self._send_motor("STEPPER_DISABLE"))
        self.btn_zero = make_button("SET ZERO", "primary", sends="SET_POSITION_ZERO <motor_id>", slot=lambda: self._send_motor("SET_POSITION_ZERO"))
        self.btn_home = make_button("HOME", "primary", sends="STEPPER_HOME <motor_id>", slot=lambda: self._send_motor("STEPPER_HOME"))
        self.btn_stop = make_button("STOP", "danger", sends="STEPPER_STOP <motor_id>", min_height=30, slot=lambda: self._send_motor("STEPPER_STOP"))
        grid.addWidget(self.btn_enable, 0, 0); grid.addWidget(self.btn_disable, 0, 1); grid.addWidget(self.btn_zero, 0, 2)
        grid.addWidget(self.btn_home, 1, 0); grid.addWidget(self.btn_stop, 1, 1, 1, 2)
        lay.addLayout(grid)
        self.motor_note = QLabel(""); self.motor_note.setWordWrap(True); self.motor_note.setMinimumWidth(1)
        self.motor_note.setStyleSheet(f"color: {AMBER}; font-size: 8pt;")
        lay.addWidget(self.motor_note)
        self.resp_motor = ResponseLine()
        lay.addWidget(self.resp_motor)
        outer.addWidget(frame)

        frame, lay = group_box("Jog (mm, relative, allowed before zero)")
        jog = QGridLayout(); jog.setSpacing(3)
        self.jog_buttons = []
        for index, delta in enumerate(JOG_MM):
            btn = make_button(f"{delta:+g} mm", "neutral", sends=f"STEPPER_MOVE_MM <motor_id> {delta:+g}", min_height=24,
                              compact=True, slot=lambda d=delta: self._jog(d))
            self.jog_buttons.append(btn)
            jog.addWidget(btn, index // 3, index % 3)
        lay.addLayout(jog)
        self.jog_note = QLabel(""); self.jog_note.setWordWrap(True); self.jog_note.setMinimumWidth(1)
        self.jog_note.setStyleSheet(f"color: {AMBER}; font-size: 8pt;")
        j_lbl = QLabel("converted onboard via stepper.lead_mm_per_rev (2 mm/rev default)")
        j_lbl.setWordWrap(True); j_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(j_lbl)
        lay.addWidget(self.jog_note)
        self.resp_jog = ResponseLine()
        lay.addWidget(self.resp_jog)
        outer.addWidget(frame)

        frame, lay = group_box("Drive settings (per motor)")
        self.speed = QDoubleSpinBox(); self.speed.setRange(0.01, MAX_SPEED_MM_S); self.speed.setDecimals(2)
        self.speed.setSingleStep(0.01); self.speed.setValue(DEFAULT_SPEED_MM_S)
        self.btn_speed = make_button("SET SPEED", "primary", sends="STEPPER_SET_SPEED <motor_id> <full-steps/s>",
                                     min_height=24, slot=self._set_speed)
        s_lbl = QLabel(f"ball-screw travel speed, up to {MAX_SPEED_MM_S:g} mm/s (stepper.max_speed_mm_s); "
                       f"sent as full-steps/s at the {LEAD_MM_PER_REV:g} mm lead")
        s_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        s_lbl.setWordWrap(True)
        lay.addWidget(hrow(with_unit(self.speed, "mm/s"), self.btn_speed, stretch_last=True))
        lay.addWidget(s_lbl)
        self.current = QDoubleSpinBox(); self.current.setRange(0.05, 3.1); self.current.setDecimals(2)
        self.current.setSingleStep(0.05); self.current.setValue(DEFAULT_CURRENT_A)
        self.btn_current = make_button("SET CURRENT", "primary", sends="STEPPER_SET_CURRENT <motor_id> <a_rms>",
                                       min_height=24, slot=self._set_current)
        c_lbl = QLabel("run current A RMS; onboard rejects what the sense resistor cannot deliver")
        c_lbl.setWordWrap(True); c_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(with_unit(self.current, "A"), self.btn_current, stretch_last=True))
        lay.addWidget(c_lbl)
        self.accel = QDoubleSpinBox(); self.accel.setRange(0.01, MAX_ACCEL_MM_S2); self.accel.setDecimals(2)
        self.accel.setSingleStep(0.5); self.accel.setValue(DEFAULT_ACCEL_MM_S2)
        self.btn_accel = make_button("SET ACCEL", "primary", sends="STEPPER_SET_ACCEL <motor_id> <full-steps/s²>",
                                     min_height=24, slot=self._set_accel)
        a_lbl = QLabel(f"trapezoid acceleration, up to {MAX_ACCEL_MM_S2:g} mm/s² "
                       "(stepper.max_accel_steps_per_s2); sent as full-steps/s²")
        a_lbl.setWordWrap(True); a_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(with_unit(self.accel, "mm/s²"), self.btn_accel, stretch_last=True))
        lay.addWidget(a_lbl)
        self.drive_now = QLabel("—"); self.drive_now.setStyleSheet(f"{MONO_CSS} color: {MUTED}; font-size: 8pt;")
        self.drive_now.setWordWrap(True); self.drive_now.setMinimumWidth(1)
        lay.addWidget(self.drive_now)
        self.resp_drive = ResponseLine()
        lay.addWidget(self.resp_drive)
        outer.addWidget(frame)

        frame, lay = group_box("Bend")
        self.bend_target = QDoubleSpinBox(); self.bend_target.setRange(-500.0, 500.0); self.bend_target.setDecimals(3)
        self.bend_target.setValue(DEFAULT_BEND_MM)
        self.bend_target.setFixedWidth(66)
        self.bend_target.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.bend_hold = QDoubleSpinBox(); self.bend_hold.setRange(0.0, 3600.0); self.bend_hold.setDecimals(1)
        self.bend_hold.setValue(DEFAULT_HOLD_S); self.bend_hold.setFixedWidth(52)
        self.bend_hold.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.btn_bend = make_button("BEND", "success", sends="STEPPER_MOVETO_MM <motor_id> <mm> <hold_s>", min_height=30, slot=self._bend)
        t_lbl = QLabel("target"); t_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        h_lbl = QLabel("hold"); h_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(t_lbl, with_unit(self.bend_target, "mm"), h_lbl, with_unit(self.bend_hold, "s"),
                           self.btn_bend))
        self.btn_pull = make_button("STANDARD PULL", "primary", sends="PULL_EXECUTE <motor_id>", min_height=26, slot=self._pull)
        p_lbl = QLabel("pulls to 2.0 mm (one revolution), holds 5 s, retracts to 0 · config pull.*; emits EVT,PULL")
        p_lbl.setWordWrap(True); p_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.btn_pull)
        lay.addWidget(p_lbl)
        self.bend_note = QLabel(""); self.bend_note.setWordWrap(True); self.bend_note.setMinimumWidth(1)
        self.bend_note.setStyleSheet(f"color: {AMBER}; font-size: 8pt;")
        lay.addWidget(self.bend_note)
        self.resp_bend = ResponseLine()
        lay.addWidget(self.resp_bend)
        outer.addWidget(frame)

        frame, lay = group_box("Recent pulls")
        self.pull_lines = [QLabel("—") for _ in range(3)]
        for lbl in self.pull_lines:
            lbl.setStyleSheet(f"{MONO_CSS} font-size: 9pt;")
            lay.addWidget(lbl)
        outer.addWidget(frame)
        outer.addStretch()
        self.update_state(self.state)

    # -- helpers -------------------------------------------------------------------
    def motor_id(self) -> int:
        value = self.selector.value()
        return int(value) if value is not None else 0

    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def _send_motor(self, verb: str) -> None:
        self._send(f"{verb} {self.motor_id()}")

    def _jog(self, delta_mm: float) -> None:
        ok, norm = validate_move_mm(delta_mm)
        if not ok:
            self.resp_jog.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_MOVE_MM {self.motor_id()} {norm}")

    def _set_speed(self) -> None:
        ok, norm = validate_speed_mm_s(self.speed.value())
        if not ok:
            self.resp_drive.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_SET_SPEED {self.motor_id()} {norm}")

    def _set_current(self) -> None:
        ok, norm = validate_current_a(self.current.value())
        if not ok:
            self.resp_drive.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_SET_CURRENT {self.motor_id()} {norm}")

    def _set_accel(self) -> None:
        ok, norm = validate_accel_mm_s2(self.accel.value())
        if not ok:
            self.resp_drive.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_SET_ACCEL {self.motor_id()} {norm}")

    def _bend(self) -> None:
        ok, norm = validate_move_mm(self.bend_target.value())
        if not ok:
            self.resp_bend.show_note(f"✖ {norm}", RED); return
        motor = self.motor_id()
        self.tracker.mark_start(motor, self.state, utc_now_iso())
        self._send(f"STEPPER_MOVETO_MM {motor} {norm} {self.bend_hold.value():g}")

    def _pull(self) -> None:
        motor = self.motor_id()
        self.tracker.mark_start(motor, self.state, utc_now_iso())
        self._send(f"PULL_EXECUTE {motor}")

    def stop_all(self) -> None:
        for motor_id in range(MOTOR_COUNT):
            self._disp.send(f"STEPPER_STOP {motor_id}", tag=self)

    # -- inputs --------------------------------------------------------------------
    def set_layout(self, layout: Layout) -> None:
        for card in self.cards:
            card.group.setText(motor_group_text(layout, card.motor_id))

    def update_state(self, state: OnboardState) -> None:
        self.state = state
        self.tracker.update(state, utc_now_iso())
        for card in self.cards:
            card.update_card(state.motor(card.motor_id), self.tracker.readout(card.motor_id, state))
        motor_id = self.motor_id()
        for card in self.cards:
            selected = card.motor_id == motor_id
            border = MOTOR_COLORS[card.motor_id] if selected else "#333"
            card.setStyleSheet("QFrame#motorCard { background: #111; border: "
                               f"{'2px' if selected else '1px'} solid {border}; border-radius: 4px; }}".replace("}}", "}"))
        self.btn_stop.setText(f"STOP M{motor_id}")
        self.btn_stop.setToolTip("stops the selected motor — Esc stops BOTH motors")
        motor = state.motor(motor_id)
        if state.have_packet and motor.present:
            words = ["enabled" if motor.enabled else "disabled",
                     "zeroed" if motor.zeroed else ("zero unknown" if motor.zeroed is None else "NOT zeroed"),
                     "moving" if motor.moving else ("holding" if motor.holding else "idle")]
            self.sel_status.setText(f"M{motor_id}: " + " · ".join(words))
        else:
            self.sel_status.setText(f"M{motor_id}: no telemetry")
        enable_reason = gating.enable_reason(state, motor_id)
        self.btn_enable.set_reason(enable_reason)
        self.motor_note.setText(f"ENABLE disabled: {enable_reason} (System tab)" if enable_reason else "")
        self.btn_disable.set_reason(gating.generic_reason(state))
        self.btn_zero.set_reason(gating.generic_reason(state))
        self.btn_stop.set_reason(gating.generic_reason(state))
        self.btn_home.set_reason(gating.motion_reason(state, motor_id, needs_zero=True))
        jog_reason = gating.motion_reason(state, motor_id, needs_zero=False)
        for btn in self.jog_buttons:
            btn.set_reason(jog_reason)
        self.jog_note.setText(f"Jog disabled: {jog_reason}" if jog_reason else "")
        self.btn_speed.set_reason(gating.generic_reason(state))
        self.btn_current.set_reason(gating.generic_reason(state))
        self.btn_accel.set_reason(gating.generic_reason(state))
        if state.have_packet and motor.present:
            amps = f"{motor.amps:.2f} A" if motor.amps is not None else "? A"
            acc = f"{motor.accel / FULL_STEPS_PER_MM:.2f} mm/s²" if motor.accel is not None else "? mm/s²"
            self.drive_now.setText(f"M{motor_id} now: {mm_s_from_hz(motor.hz):.2f} mm/s · {amps} · {acc}")
        else:
            self.drive_now.setText("—")
        bend_reason = gating.motion_reason(state, motor_id, needs_zero=True)
        self.btn_bend.set_reason(bend_reason)
        self.btn_pull.set_reason(bend_reason)
        self.bend_note.setText(f"BEND / STANDARD PULL disabled: {bend_reason}" if bend_reason else "")

    def on_pull_event(self, ev: PullEvent) -> None:
        # Freeze the mm value at arrival: steps_moved is in µsteps at the
        # divisor the PULL ran at (carried in the event since 2026-08-30;
        # live divisor is the fallback for old firmware). Re-deriving on
        # render would silently rewrite history after STEPPER_SET_MICROSTEP.
        us = ev.microstep or self.state.motor(ev.motor_id).microstep or 4
        mm = ev.steps_moved / (200.0 * us / 2.0)
        self._pulls.appendleft((ev, mm))
        for lbl, entry in zip(self.pull_lines, list(self._pulls) + [None] * 3):
            if entry is None:
                lbl.setText("—"); continue
            pull, pull_mm = entry
            samples = "|".join(str(s) for s in pull.samples) or "-"
            lbl.setText(f"{pull.start_ts[11:19] if len(pull.start_ts) > 18 else pull.start_ts}  M{pull.motor_id}  "
                        f"pull #{pull.pull_id}  {pull_mm:+.2f} mm  hold {pull.hold_s:.1f} s  S{samples}")
            lbl.setStyleSheet(f"{MONO_CSS} font-size: 9pt; color: {MOTOR_COLORS[pull.motor_id % 2]};")

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb in ("STEPPER_MOVE", "STEPPER_MOVE_MM"):
            self.resp_jog.show_response(cmd, resp, ms)
        elif verb in ("STEPPER_SET_SPEED", "STEPPER_SET_CURRENT", "STEPPER_SET_ACCEL"):
            self.resp_drive.show_response(cmd, resp, ms)
        elif verb in ("STEPPER_MOVETO", "STEPPER_MOVETO_MM", "PULL_EXECUTE", "STEPPER_BEND"):
            self.resp_bend.show_response(cmd, resp, ms)
        else:
            self.resp_motor.show_response(cmd, resp, ms)
