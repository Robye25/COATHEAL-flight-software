"""Debug tab: is the motor really moving? (owner request, 2026-08-28)

Polls `MOTOR_DEBUG <id>` at a few Hz through the dispatcher's quiet path,
decodes the chip's motion-truth registers, turns ΔMSCNT / ΔXACTUAL into
full-steps/s, rev/s and mm/s, plots them, and states a verdict.
"""
from __future__ import annotations

import time
from typing import Dict, List, Optional

import pyqtgraph as pg
from PyQt6.QtCore import QSettings, Qt, QTimer
from PyQt6.QtWidgets import (
    QDoubleSpinBox, QGridLayout, QHBoxLayout, QLabel, QScrollArea, QSpinBox, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse
from . import gating
from .dispatch import CommandDispatcher
from .motor_debug import (
    DEFAULT_MM_PER_REV, FULL_STEPS_PER_REV, MotionEstimate, MotionEstimator, MotorDebugSample,
)
from .state import OnboardState
from .widgets import (
    AMBER, GRAY, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, group_box, hrow, make_button,
    soft_breaks,
)

_COLORS = {"green": GREEN, "amber": AMBER, "red": RED, "gray": MUTED}
REGISTER_ROWS = [
    ("mscnt", "MSCNT (sine-table index, 256/full step)"), ("xactual", "XACTUAL (1/256 steps)"),
    ("xtarget", "XTARGET"), ("vactual", "VACTUAL (ramp velocity)"), ("tstep", "TSTEP"),
    ("sw_pos", "firmware position (µsteps)"), ("sw_tgt", "firmware target"), ("sw_hz", "configured Hz"),
    ("usteps", "microstep divisor"), ("stst", "standstill (stst)"), ("cs_actual", "current scale (CS_ACTUAL)"),
    ("sg_result", "stallGuard result"), ("drv_enn", "DRV_ENN (1 = power stage off)"), ("toff", "TOFF (0 = chopper off)"),
    ("sd_mode", "SD_MODE (1 = STEP/DIR strap)"), ("enabled", "firmware enabled"), ("moving", "firmware moving"),
    ("pulses", "pulses issued"), ("missed", "missed deadlines"), ("faults", "fault flags"),
    ("gstat", "GSTAT (bit 0 = reset since configured)"), ("resets", "chip resets since boot"),
    ("stealth", "stealthChop active"), ("pwm_scale_sum", "PWM amplitude 0–255 (255 = cannot reach current)"),
]


class DebugTab(QScrollArea):
    def __init__(self, dispatcher: CommandDispatcher, settings: Optional[QSettings] = None, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self._settings = settings
        self.state = OnboardState()
        self._clock = time.monotonic  # tests substitute a fake clock
        self._t0 = self._clock()
        mm = DEFAULT_MM_PER_REV
        if settings is not None:
            try:
                mm = float(settings.value("debug/mm_per_rev", DEFAULT_MM_PER_REV))
            except (TypeError, ValueError):
                mm = DEFAULT_MM_PER_REV
        self.estimator = MotionEstimator(mm_per_rev=mm)
        self.last_sample: Optional[MotorDebugSample] = None
        self.last_estimate: Optional[MotionEstimate] = None
        self.polls_sent = 0
        self.polls_answered = 0

        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        frame, lay = group_box("Motion truth — live TMC5160 registers")
        self.selector = Segmented([("M0", 0), ("M1", 1)], current=0)
        self.selector.valueChanged.connect(lambda _v: self._restart_estimator())
        self.btn_probe = make_button("START PROBE", "success", sends="MOTOR_DEBUG <motor_id> (polled)", min_height=28,
                                     slot=self.toggle_probe)
        self.btn_once = make_button("READ ONCE", "neutral", sends="MOTOR_DEBUG <motor_id>", min_height=28, slot=self.read_once)
        lay.addWidget(hrow(self.selector, self.btn_probe, self.btn_once))
        self.interval = QSpinBox(); self.interval.setRange(200, 5000); self.interval.setSingleStep(100); self.interval.setValue(500)
        self.interval.setSuffix(" ms")
        self.mm_per_rev = QDoubleSpinBox(); self.mm_per_rev.setRange(0.01, 100.0); self.mm_per_rev.setDecimals(3)
        self.mm_per_rev.setValue(mm); self.mm_per_rev.setSuffix(" mm/rev")
        self.mm_per_rev.valueChanged.connect(self._mm_changed)
        il = QLabel("poll every"); il.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        ml = QLabel("ball-screw lead"); ml.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(il, self.interval, ml, self.mm_per_rev, stretch_last=True))
        note = QLabel("The telemetry position is the firmware's counter and advances even when nothing turns. "
                      "MSCNT is the chip's sine-table index and moves only when the coils are stepped: "
                      "256 counts per full step, whatever the microstep setting. Expected at 100 Hz: 0.5 rev/s; "
                      "with a 1.5 mm lead a BEND of 800 µsteps (µ4 = 1 revolution) is ~1.5 mm in 2 s.")
        note.setWordWrap(True); note.setMinimumWidth(1); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        outer.addWidget(frame)

        frame, lay = group_box("Verdict")
        self.verdict = QLabel("no sample yet")
        self.verdict.setWordWrap(True); self.verdict.setMinimumWidth(1)
        self.verdict.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {MUTED}; font-size: 11pt;")
        lay.addWidget(self.verdict)
        grid = QGridLayout(); grid.setHorizontalSpacing(10); grid.setVerticalSpacing(3)
        self._derived: Dict[str, QLabel] = {}
        for row, (key, label) in enumerate((("seq", "sequencer rate"), ("ramp", "ramp-generator rate"),
                                            ("rev", "revolutions"), ("mm", "linear speed"),
                                            ("travel", "travel since probe start"), ("polls", "polls"))):
            k = QLabel(label); k.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
            v = QLabel("—"); v.setStyleSheet(MONO_CSS); v.setMinimumWidth(1)
            grid.addWidget(k, row, 0); grid.addWidget(v, row, 1)
            self._derived[key] = v
        grid.setColumnStretch(1, 1)
        lay.addLayout(grid)
        outer.addWidget(frame)

        frame, lay = group_box("MSCNT and XACTUAL over time")
        pg.setConfigOption("background", "#0d0d0d")
        self.plot = pg.PlotWidget()
        self.plot.setMinimumHeight(150)
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.setLabel("bottom", "s since probe start")
        self.plot.addLegend(offset=(6, 6))
        self._c_mscnt = self.plot.plot([], [], pen=pg.mkPen("#f1c40f", width=1.6), name="MSCNT")
        self._c_xact = self.plot.plot([], [], pen=pg.mkPen("#3498db", width=1.6), name="XACTUAL / 256 (full steps)")
        self._series_t: List[float] = []
        self._series_mscnt: List[float] = []
        self._series_xact: List[float] = []
        lay.addWidget(self.plot)
        outer.addWidget(frame)

        frame, lay = group_box("Registers (decoded)")
        grid = QGridLayout(); grid.setHorizontalSpacing(10); grid.setVerticalSpacing(2)
        self._regs: Dict[str, QLabel] = {}
        for row, (key, label) in enumerate(REGISTER_ROWS):
            k = QLabel(label); k.setStyleSheet(f"color: {MUTED}; font-size: 8pt;"); k.setMinimumWidth(1)
            v = QLabel("—"); v.setStyleSheet(MONO_CSS); v.setMinimumWidth(1)
            grid.addWidget(k, row, 0); grid.addWidget(v, row, 1)
            self._regs[key] = v
        grid.setColumnStretch(0, 1)
        lay.addLayout(grid)
        self.raw = QLabel("—"); self.raw.setWordWrap(True); self.raw.setMinimumWidth(1)
        self.raw.setStyleSheet(f"{MONO_CSS} color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.raw)
        self.resp = ResponseLine()
        lay.addWidget(self.resp)
        outer.addWidget(frame)
        outer.addStretch()

        self._timer = QTimer(self); self._timer.timeout.connect(self._poll)
        self._disp.quiet_response.connect(self.on_quiet_response)
        self.update_state(self.state)

    # -- control -----------------------------------------------------------------
    def motor_id(self) -> int:
        value = self.selector.value()
        return int(value) if value is not None else 0

    @property
    def probing(self) -> bool:
        return self._timer.isActive()

    def toggle_probe(self) -> None:
        self.stop_probe() if self.probing else self.start_probe()

    def start_probe(self) -> None:
        self._restart_estimator()
        self._timer.start(int(self.interval.value()))
        self.btn_probe.setText("STOP PROBE")
        self._poll()

    def stop_probe(self) -> None:
        self._timer.stop()
        self.btn_probe.setText("START PROBE")

    def read_once(self) -> None:
        self._disp.send(f"MOTOR_DEBUG {self.motor_id()}", tag=self, quiet=True)
        self.polls_sent += 1

    def _poll(self) -> None:
        if self._disp.silence:
            self.stop_probe()
            self.resp.show_note("✖ probe stopped: radio silence", RED)
            return
        self._disp.send(f"MOTOR_DEBUG {self.motor_id()}", tag=self, quiet=True)
        self.polls_sent += 1

    def _restart_estimator(self) -> None:
        self.estimator.reset()
        self._t0 = self._clock()
        self._series_t.clear(); self._series_mscnt.clear(); self._series_xact.clear()
        self._c_mscnt.setData([], []); self._c_xact.setData([], [])
        self._derived["travel"].setText("—")

    def _mm_changed(self, value: float) -> None:
        self.estimator.mm_per_rev = float(value)
        if self._settings is not None:
            self._settings.setValue("debug/mm_per_rev", float(value))

    # -- inputs ------------------------------------------------------------------
    def update_state(self, state: OnboardState) -> None:
        self.state = state
        reason = gating.generic_reason(state)
        self.btn_probe.set_reason(reason)
        self.btn_once.set_reason(reason)
        if reason and self.probing:
            self.stop_probe()

    def on_quiet_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        if not resp.ok:
            self.resp.show_response(cmd, resp, ms)
            self.verdict.setText(soft_breaks(f"no reading: {resp.error or resp.raw}"))
            self.verdict.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {RED}; font-size: 11pt;")
            return
        self.polls_answered += 1
        sample = MotorDebugSample.parse(resp.body, self._clock())
        if sample.motor != self.motor_id():
            return
        self.last_sample = sample
        est = self.estimator.add(sample)
        self.last_estimate = est
        self.resp.show_response(cmd, CommandResponse(ok=True, command=cmd, body=f"{len(resp.body)} chars", raw=""), ms)
        self._render(sample, est)

    def _render(self, s: MotorDebugSample, e: MotionEstimate) -> None:
        self.verdict.setText(soft_breaks(e.verdict))
        self.verdict.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {_COLORS.get(e.color, MUTED)}; font-size: 11pt;")
        self._derived["seq"].setText(f"{e.sequencer_full_steps_s:6.2f} full-steps/s  (from ΔMSCNT)")
        self._derived["ramp"].setText(f"{e.ramp_full_steps_s:+6.2f} full-steps/s  (from ΔXACTUAL)")
        self._derived["rev"].setText(f"{e.rev_s:7.4f} rev/s  ({e.rev_s * 60:.2f} rpm)")
        self._derived["mm"].setText(f"{e.mm_s:7.4f} mm/s  ({e.mm_s * 60:.2f} mm/min at {self.estimator.mm_per_rev:g} mm/rev)")
        self._derived["travel"].setText(f"{e.travel_full_steps:8.1f} full steps = {e.travel_full_steps / FULL_STEPS_PER_REV:.3f} rev = {e.travel_mm:.3f} mm")
        self._derived["polls"].setText(f"{self.polls_answered} / {self.polls_sent} answered · window {e.samples} samples")
        for key, label in self._regs.items():
            if key == "faults":
                label.setText(" ".join(s.faults) if s.faults else "none")
                label.setStyleSheet(f"{MONO_CSS} color: {RED if s.faults else GREEN};")
                continue
            value = getattr(s, key, None)
            if value is None:
                value = s.raw.get(key)
            text = "—" if value is None else (str(int(value)) if isinstance(value, bool) else str(value))
            label.setText(text)
            color = MONO_CSS
            if key == "stst":
                color += f" color: {MUTED if s.stst else GREEN};"
            elif key == "drv_enn":
                color += f" color: {RED if s.drv_enn else GREEN};"
            elif key == "sd_mode":
                color += f" color: {RED if s.sd_mode else GREEN};"
            elif key == "toff":
                color += f" color: {RED if s.toff == 0 else GREEN};"
            label.setStyleSheet(color)
        self.raw.setText(soft_breaks(";".join(f"{k}={v}" for k, v in s.raw.items())))
        t = s.t - self._t0
        self._series_t.append(t)
        self._series_mscnt.append(float(s.mscnt) if s.mscnt is not None else float("nan"))
        self._series_xact.append(float(s.xactual) / 256.0 if s.xactual is not None else float("nan"))
        if len(self._series_t) > 600:
            for series in (self._series_t, self._series_mscnt, self._series_xact):
                del series[0]
        self._c_mscnt.setData(self._series_t, self._series_mscnt)
        self._c_xact.setData(self._series_t, self._series_xact)
