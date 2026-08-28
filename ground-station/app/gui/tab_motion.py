"""Motion tab: the ascent bend (redesign spec §5.4).

Two always-visible motor cards, a selector that drives the shared
controls, jog, BEND (STEPPER_MOVETO with hold), STANDARD PULL
(PULL_EXECUTE), and the resistance-before/after readout that confirms a
bend. Every control that cannot succeed is disabled with its reason.
"""
from __future__ import annotations

from collections import deque
from typing import Deque, List, Optional

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QAbstractSpinBox, QDoubleSpinBox, QFrame, QGridLayout, QHBoxLayout, QLabel, QScrollArea,
    QSpinBox, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, PullEvent, validate_speed_hz, validate_stepper_move
from ..telemetry_log import utc_now_iso
from . import gating
from .bend_tracker import BendTracker
from .dispatch import CommandDispatcher
from .state import MOTOR_COUNT, MOTOR_SAMPLES, MotorState, OnboardState
from .widgets import (
    AMBER, BLUE, GRAY, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, StatusDot,
    group_box, hrow, make_button,
)

MOTOR_COLORS = ("#2ecc71", "#e67e22")
JOG_STEPS = (-1000, -100, -10, 10, 100, 1000)
DEFAULT_SPEED_HZ = 100
DEFAULT_BEND_USTEPS = 800
DEFAULT_HOLD_S = 5.0


class MotorCard(QFrame):
    def __init__(self, motor_id: int, parent=None):
        super().__init__(parent)
        self.motor_id = motor_id
        self.setObjectName("motorCard")
        self.setStyleSheet("QFrame#motorCard { background: #111; border: 1px solid #333; border-radius: 4px; }")
        lay = QVBoxLayout(self); lay.setContentsMargins(8, 6, 8, 6); lay.setSpacing(3)
        samples = MOTOR_SAMPLES[motor_id]
        title = QLabel(f"M{motor_id}")
        title.setStyleSheet(f"font-weight: bold; font-size: 12pt; color: {MOTOR_COLORS[motor_id]}; border: none;")
        sub = QLabel(f"S{samples[0]}–S{samples[-1]}")
        sub.setStyleSheet(f"color: {MUTED}; font-size: 9pt; border: none;")
        lay.addWidget(hrow(title, sub, stretch_last=True))
        dots = QHBoxLayout(); dots.setSpacing(2)
        self.dots = {}
        for key in ("EN", "ZERO", "MOV", "HOLD", "OK"):
            lbl = QLabel(key); lbl.setStyleSheet(f"color: {MUTED}; font-size: 7pt; border: none;")
            dot = StatusDot(8); dot.set_color(GRAY)
            dots.addWidget(lbl); dots.addWidget(dot); dots.addSpacing(3)
            self.dots[key] = dot
        dots.addStretch()
        lay.addLayout(dots)
        self.pos = self._kv(lay, "pos / tgt")
        self.speed = self._kv(lay, "speed")
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
        self.pos.setText(f"{motor.position} / {motor.target} µst")
        self.speed.setText(f"{motor.hz:.0f} Hz · µ{motor.microstep}")
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
            self.r_delta.setStyleSheet(f"{MONO_CSS} border: none; color: {GREEN if abs(readout.delta_pct) >= 0.5 else AMBER};")


class MotionTab(QScrollArea):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self.state = OnboardState()
        self.tracker = BendTracker()
        self._pulls: Deque[PullEvent] = deque(maxlen=3)

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
        self.resp_motor = ResponseLine()
        lay.addWidget(self.resp_motor)
        outer.addWidget(frame)

        frame, lay = group_box("Jog (relative, allowed before zero)")
        jog = QGridLayout(); jog.setSpacing(3)
        self.jog_buttons = []
        for index, delta in enumerate(JOG_STEPS):
            btn = make_button(f"{delta:+d}", "neutral", sends=f"STEPPER_MOVE <motor_id> {delta:+d}", min_height=24,
                              compact=True, slot=lambda d=delta: self._jog(d))
            self.jog_buttons.append(btn)
            jog.addWidget(btn, index // 3, index % 3)
        lay.addLayout(jog)
        self.speed = QSpinBox(); self.speed.setRange(1, 100); self.speed.setValue(DEFAULT_SPEED_HZ); self.speed.setSuffix(" Hz")
        self.btn_speed = make_button("SET SPEED", "primary", sends="STEPPER_SET_SPEED <motor_id> <hz>", min_height=24, slot=self._set_speed)
        lbl = QLabel("full-step Hz, 1–100 (pull.max_step_hz)"); lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lbl.setWordWrap(True)
        lay.addWidget(hrow(self.speed, self.btn_speed, stretch_last=True))
        lay.addWidget(lbl)
        self.resp_jog = ResponseLine()
        lay.addWidget(self.resp_jog)
        outer.addWidget(frame)

        frame, lay = group_box("Bend")
        self.bend_target = QSpinBox(); self.bend_target.setRange(-200000, 200000); self.bend_target.setValue(DEFAULT_BEND_USTEPS)
        self.bend_target.setSuffix(" µst"); self.bend_target.setFixedWidth(84)
        self.bend_target.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.bend_hold = QDoubleSpinBox(); self.bend_hold.setRange(0.0, 3600.0); self.bend_hold.setDecimals(1)
        self.bend_hold.setValue(DEFAULT_HOLD_S); self.bend_hold.setSuffix(" s"); self.bend_hold.setFixedWidth(64)
        self.bend_hold.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.btn_bend = make_button("BEND", "success", sends="STEPPER_MOVETO <motor_id> <target> <hold_s>", min_height=30, slot=self._bend)
        t_lbl = QLabel("target"); t_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        h_lbl = QLabel("hold"); h_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(t_lbl, self.bend_target, h_lbl, self.bend_hold, self.btn_bend))
        self.btn_pull = make_button("STANDARD PULL", "primary", sends="PULL_EXECUTE <motor_id>", min_height=26, slot=self._pull)
        p_lbl = QLabel("config pull: pull.travel_full_steps × µstep, hold pull.hold_s; emits EVT,PULL")
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

    def _jog(self, delta: int) -> None:
        ok, norm = validate_stepper_move(delta)
        if not ok:
            self.resp_jog.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_MOVE {self.motor_id()} {norm}")

    def _set_speed(self) -> None:
        ok, norm = validate_speed_hz(self.speed.value())
        if not ok:
            self.resp_jog.show_note(f"✖ {norm}", RED); return
        self._send(f"STEPPER_SET_SPEED {self.motor_id()} {norm}")

    def _bend(self) -> None:
        ok, norm = validate_stepper_move(self.bend_target.value())
        if not ok:
            self.resp_bend.show_note(f"✖ {norm}", RED); return
        motor = self.motor_id()
        self.tracker.mark_start(motor, self.state, utc_now_iso())
        self._send(f"STEPPER_MOVETO {motor} {norm} {self.bend_hold.value():g}")

    def _pull(self) -> None:
        motor = self.motor_id()
        self.tracker.mark_start(motor, self.state, utc_now_iso())
        self._send(f"PULL_EXECUTE {motor}")

    def stop_all(self) -> None:
        for motor_id in range(MOTOR_COUNT):
            self._disp.send(f"STEPPER_STOP {motor_id}", tag=self)

    # -- inputs --------------------------------------------------------------------
    def update_state(self, state: OnboardState) -> None:
        self.state = state
        self.tracker.update(state, utc_now_iso())
        for card in self.cards:
            card.update_card(state.motor(card.motor_id), self.tracker.readout(card.motor_id, state))
        motor_id = self.motor_id()
        motor = state.motor(motor_id)
        if state.have_packet and motor.present:
            words = ["enabled" if motor.enabled else "disabled",
                     "zeroed" if motor.zeroed else ("zero unknown" if motor.zeroed is None else "NOT zeroed"),
                     "moving" if motor.moving else ("holding" if motor.holding else "idle")]
            self.sel_status.setText(f"M{motor_id}: " + " · ".join(words))
        else:
            self.sel_status.setText(f"M{motor_id}: no telemetry")
        self.btn_enable.set_reason(gating.enable_reason(state, motor_id))
        self.btn_disable.set_reason(gating.generic_reason(state))
        self.btn_zero.set_reason(gating.generic_reason(state))
        self.btn_stop.set_reason(gating.generic_reason(state))
        self.btn_home.set_reason(gating.motion_reason(state, motor_id, needs_zero=True))
        jog_reason = gating.motion_reason(state, motor_id, needs_zero=False)
        for btn in self.jog_buttons:
            btn.set_reason(jog_reason)
        self.btn_speed.set_reason(gating.generic_reason(state))
        bend_reason = gating.motion_reason(state, motor_id, needs_zero=True)
        self.btn_bend.set_reason(bend_reason)
        self.btn_pull.set_reason(bend_reason)
        self.bend_note.setText(f"BEND / STANDARD PULL disabled: {bend_reason}" if bend_reason else "")

    def on_pull_event(self, ev: PullEvent) -> None:
        self._pulls.appendleft(ev)
        for lbl, pull in zip(self.pull_lines, list(self._pulls) + [None] * 3):
            if pull is None:
                lbl.setText("—"); continue
            samples = "|".join(str(s) for s in pull.samples) or "-"
            lbl.setText(f"{pull.start_ts[11:19] if len(pull.start_ts) > 18 else pull.start_ts}  M{pull.motor_id}  "
                        f"pull #{pull.pull_id}  {pull.steps_moved:+d} µst  hold {pull.hold_s:.1f} s  S{samples}")
            lbl.setStyleSheet(f"{MONO_CSS} font-size: 9pt; color: {MOTOR_COLORS[pull.motor_id % 2]};")

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb in ("STEPPER_MOVE", "STEPPER_SET_SPEED"):
            self.resp_jog.show_response(cmd, resp, ms)
        elif verb in ("STEPPER_MOVETO", "PULL_EXECUTE", "STEPPER_BEND"):
            self.resp_bend.show_response(cmd, resp, ms)
        else:
            self.resp_motor.show_response(cmd, resp, ms)
