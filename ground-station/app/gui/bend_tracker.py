"""Resistance before/after a bend, per motor (redesign spec §5.4).

The owner confirms a bend by watching the specimen resistance. This
module remembers the monitored specimens' resistance when a bend starts
-- explicitly when the operator presses BEND / STANDARD PULL, and
automatically on the motor's idle->moving edge as a fallback -- and reports
the relative change since then. Pure; fed with `OnboardState` snapshots.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

from .state import MOTOR_COUNT, OnboardState


@dataclass(frozen=True)
class BendReadout:
    motor_id: int
    sample: Optional[int]           # monitored specimen the numbers refer to
    r_now: Optional[float]
    r_start: Optional[float]
    delta_pct: Optional[float]
    started_utc: Optional[str]
    source: str = ""                # "operator" | "motion" | ""


class BendTracker:
    def __init__(self) -> None:
        self._start_r: Dict[int, Dict[int, float]] = {}
        self._start_ts: Dict[int, str] = {}
        self._start_src: Dict[int, str] = {}
        self._was_moving: Dict[int, bool] = {m: False for m in range(MOTOR_COUNT)}

    @staticmethod
    def monitored_samples(state: OnboardState, motor_id: int) -> List[int]:
        motor = state.motor(motor_id)
        return [s for s in motor.samples
                if s < len(state.sample_resistance) and state.sample_resistance[s] is not None]

    def mark_start(self, motor_id: int, state: OnboardState, ts_utc: str,
                   source: str = "operator") -> None:
        readings = {s: state.sample_resistance[s] for s in self.monitored_samples(state, motor_id)}
        self._start_r[motor_id] = {s: float(r) for s, r in readings.items() if r is not None}
        self._start_ts[motor_id] = ts_utc
        self._start_src[motor_id] = source

    def update(self, state: OnboardState, ts_utc: str) -> None:
        """Auto-mark on the idle->moving edge unless the operator marked a
        start within this motion already."""
        for motor in state.motors:
            moving = motor.moving or motor.holding
            if moving and not self._was_moving.get(motor.motor_id, False):
                if self._start_src.get(motor.motor_id) != "operator" or motor.motor_id not in self._start_r:
                    self.mark_start(motor.motor_id, state, ts_utc, source="motion")
            if not moving and self._was_moving.get(motor.motor_id, False):
                # Motion finished: the operator mark has served its purpose;
                # the next motion may auto-mark again.
                if self._start_src.get(motor.motor_id) == "operator":
                    self._start_src[motor.motor_id] = "operator-done"
            self._was_moving[motor.motor_id] = moving

    def readout(self, motor_id: int, state: OnboardState) -> BendReadout:
        monitored = self.monitored_samples(state, motor_id)
        starts = self._start_r.get(motor_id, {})
        sample: Optional[int] = None
        for candidate in monitored:
            if candidate in starts:
                sample = candidate
                break
        if sample is None and monitored:
            sample = monitored[0]
        r_now = state.sample_resistance[sample] if sample is not None else None
        r_start = starts.get(sample) if sample is not None else None
        delta = None
        if r_now is not None and r_start not in (None, 0.0):
            delta = (r_now - r_start) / r_start * 100.0
        return BendReadout(motor_id, sample, r_now, r_start, delta,
                           self._start_ts.get(motor_id), self._start_src.get(motor_id, "").replace("-done", ""))
