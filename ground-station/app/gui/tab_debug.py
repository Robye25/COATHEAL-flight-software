"""Debug tab: the TMC5160 drivers' own registers, live (owner request,
2026-08-28; slimmed 2026-08-29 — the decoded registers are the value here,
so they lead; the estimator survives as one verdict + one rate line).

Polls `MOTOR_DEBUG <id>` through the dispatcher's quiet path, decodes the
chip's motion-truth and health registers, and states a one-line verdict.
The ball-screw lead for the mm/s rate is the onboard's configured 2 mm/rev
(stepper.lead_mm_per_rev) — no local override.
"""
from __future__ import annotations

import time
from typing import Dict, Optional

from PyQt6.QtCore import QSettings, Qt, QTimer
from PyQt6.QtWidgets import QGridLayout, QLabel, QScrollArea, QSpinBox, QVBoxLayout, QWidget

from ..protocol import CommandResponse
from . import gating
from .dispatch import CommandDispatcher
from .motor_debug import MotionEstimate, MotionEstimator, MotorDebugSample
from .state import OnboardState
from .widgets import (
    AMBER, GREEN, MONO_CSS, MUTED, RED, ResponseLine, Segmented, group_box, hrow, make_button,
    soft_breaks,
)

_COLORS = {"green": GREEN, "amber": AMBER, "red": RED, "gray": MUTED}
REGISTER_ROWS = [
    ("mscnt", "MSCNT (sine-table index, 256/full step)"), ("xactual", "XACTUAL (1/256 steps)"),
    ("xtarget", "XTARGET"), ("vactual", "VACTUAL (ramp velocity)"), ("tstep", "TSTEP"),
    ("sw_pos", "firmware position (µsteps)"), ("sw_tgt", "firmware target"), ("sw_hz", "configured Hz"),
    ("us", "firmware microstep divisor (pulse = 1/us full step)"),
    ("usteps", "chip MRES resolution (µsteps per full step; 256 = native)"),
    ("stst", "standstill (stst)"), ("cs_actual", "current scale (CS_ACTUAL)"),
    ("sg_result", "stallGuard result"), ("drv_enn", "DRV_ENN (1 = power stage off)"),
    ("toff", "TOFF (0 = chopper off)"), ("sd_mode", "SD_MODE (1 = STEP/DIR strap)"),
    ("ot", "ot — die ≥150 °C over-temperature shutdown"),
    ("otpw", "otpw — die ≥120 °C pre-warning"),
    ("enabled", "firmware enabled"), ("moving", "firmware moving"),
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
        self.estimator = MotionEstimator()
        self.last_sample: Optional[MotorDebugSample] = None
        self.last_estimate: Optional[MotionEstimate] = None
        self.polls_sent = 0
        self.polls_answered = 0

        inner = QWidget(); self.setWidget(inner)
        outer = QVBoxLayout(inner); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(8)

        frame, lay = group_box("TMC5160 driver — live registers (MOTOR_DEBUG)")
        self.selector = Segmented([("M0", 0), ("M1", 1)], current=0)
        self.selector.valueChanged.connect(lambda _v: self._restart_estimator())
        self.btn_probe = make_button("START PROBE", "success", sends="MOTOR_DEBUG <motor_id> (polled)", min_height=28,
                                     slot=self.toggle_probe)
        self.btn_once = make_button("READ ONCE", "neutral", sends="MOTOR_DEBUG <motor_id>", min_height=28, slot=self.read_once)
        self.interval = QSpinBox(); self.interval.setRange(200, 5000); self.interval.setSingleStep(100); self.interval.setValue(500)
        self.interval.setSuffix(" ms")
        il = QLabel("poll every"); il.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(self.selector, self.btn_probe, self.btn_once))
        lay.addWidget(hrow(il, self.interval, stretch_last=True))

        # Driver health at a glance: thermal state, faults, resets, PWM
        # saturation. This is the line the operator scans first.
        self.health = QLabel("no sample yet")
        self.health.setWordWrap(True); self.health.setMinimumWidth(1)
        self.health.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {MUTED}; font-size: 10pt;")
        lay.addWidget(self.health)

        self.verdict = QLabel("—")
        self.verdict.setWordWrap(True); self.verdict.setMinimumWidth(1)
        self.verdict.setStyleSheet(f"{MONO_CSS} color: {MUTED};")
        lay.addWidget(self.verdict)
        self.rate = QLabel("—")
        self.rate.setWordWrap(True); self.rate.setMinimumWidth(1)
        self.rate.setStyleSheet(f"{MONO_CSS} color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self.rate)

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
        if self._disp.silence:
            self.resp.show_note("✖ radio silence — RADIO RESUME first", RED)
            return
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
            self.health.setText(soft_breaks(f"no reading: {resp.error or resp.raw}"))
            self.health.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {RED}; font-size: 10pt;")
            return
        sample = MotorDebugSample.parse(resp.body, self._clock())
        if sample.motor != self.motor_id():
            return
        self.polls_answered += 1
        self.last_sample = sample
        est = self.estimator.add(sample)
        self.last_estimate = est
        self.resp.show_response(cmd, CommandResponse(ok=True, command=cmd, body=f"{len(resp.body)} chars", raw=""), ms)
        self._render(sample, est)

    def _health_line(self, s: MotorDebugSample) -> tuple:
        ot = s.raw.get("ot") == "1"
        otpw = s.raw.get("otpw") == "1"
        parts = []
        color = GREEN
        if ot:
            parts.append("OVER-TEMPERATURE ≥150 °C — outputs cut")
            color = RED
        elif otpw:
            parts.append("die ≥120 °C pre-warning")
            color = AMBER
        else:
            parts.append("die < 120 °C")
        other = [f for f in s.faults if f not in ("ot", "otpw")]
        if other:
            parts.append("faults: " + " ".join(other))
            color = RED
        if s.resets:
            parts.append(f"chip resets ×{s.resets}")
            if color == GREEN:
                color = AMBER
        if s.stealth and s.pwm_scale_sum is not None and s.pwm_scale_sum >= 255:
            parts.append("current regulator SATURATED (PWM 255)")
            if color == GREEN:
                color = AMBER
        return " · ".join(parts), color

    def _render(self, s: MotorDebugSample, e: MotionEstimate) -> None:
        text, color = self._health_line(s)
        self.health.setText(soft_breaks(f"M{s.motor}: {text}"))
        self.health.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {color}; font-size: 10pt;")
        self.verdict.setText(soft_breaks(e.verdict))
        self.verdict.setStyleSheet(f"{MONO_CSS} color: {_COLORS.get(e.color, MUTED)};")
        self.rate.setText(
            f"{e.sequencer_full_steps_s:.2f} full-steps/s (ΔMSCNT) · {e.ramp_full_steps_s:+.2f} full-steps/s (ΔXACTUAL) · "
            f"{e.rev_s:.4f} rev/s · {e.mm_s:.3f} mm/s · travel {e.travel_mm:.3f} mm · "
            f"{self.polls_answered}/{self.polls_sent} polls")
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
            elif key == "ot":
                color += f" color: {RED if s.raw.get('ot') == '1' else GREEN};"
            elif key == "otpw":
                color += f" color: {AMBER if s.raw.get('otpw') == '1' else GREEN};"
            label.setStyleSheet(color)
        self.raw.setText(soft_breaks(";".join(f"{k}={v}" for k, v in s.raw.items())))
