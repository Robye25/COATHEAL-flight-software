"""One immutable snapshot of what the ground station currently knows about
the onboard, derived from the last telemetry packet plus ground-side
facts (radio silence, link age). Every panel reads this instead of
re-deriving its own view of the packet, and `gating.py` decides from it
which controls can succeed. No Qt here.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional

from ..protocol import TelemetryPacket

MOTOR_COUNT = 2
HEATER_COUNT = 6
SAMPLE_COUNT = 8
# Heater i is fed by sample i (heater.temperature_channels=0..5 in the
# flight config). Motor 0 pulls samples 0..3, motor 1 pulls 4..7.
HEATER_SAMPLE = tuple(range(HEATER_COUNT))
MOTOR_SAMPLES = ((0, 1, 2, 3), (4, 5, 6, 7))


@dataclass(frozen=True)
class MotorState:
    motor_id: int
    present: bool = False          # a STEPPER<n>= segment was in the frame
    enabled: bool = False
    zeroed: Optional[bool] = None  # None: firmware did not say
    moving: bool = False
    holding: bool = False
    healthy: bool = False
    position: int = 0
    target: int = 0
    hz: float = 0.0
    microstep: int = 0
    hold_s: float = 0.0
    pulses: int = 0
    source: str = ""
    seq_name: str = ""
    seq_state: str = ""

    @property
    def samples(self) -> tuple:
        return MOTOR_SAMPLES[self.motor_id] if 0 <= self.motor_id < MOTOR_COUNT else ()


@dataclass(frozen=True)
class OnboardState:
    have_packet: bool = False
    session_id: str = ""
    seq: int = 0
    mode: str = ""                 # STANDBY / RUN / SAFE / ""
    phase: str = ""
    status_tokens: FrozenSet[str] = frozenset()
    component_state: Dict[str, str] = field(default_factory=dict)
    sample_temps: List[Optional[float]] = field(default_factory=lambda: [None] * SAMPLE_COUNT)
    sample_resistance: List[Optional[float]] = field(default_factory=lambda: [None] * SAMPLE_COUNT)
    heater_duty: List[float] = field(default_factory=lambda: [0.0] * HEATER_COUNT)
    ambient_temp_c: Optional[float] = None
    ambient_pressure_mbar: Optional[float] = None
    uv: Optional[float] = None
    rtc_valid: bool = False
    motors: List[MotorState] = field(default_factory=lambda: [MotorState(i) for i in range(MOTOR_COUNT)])
    fallback: Optional[bool] = None
    link_loss_s: Optional[float] = None
    energy_wh: Optional[float] = None
    budget_wh: Optional[float] = None
    budget_exhausted: Optional[bool] = None
    heaters_active: Optional[int] = None
    queue_depth: Optional[int] = None
    plan_state: Optional[str] = None
    # Ground-side facts.
    silence: bool = False
    link_age_s: Optional[float] = None
    # Replay of the onboard backlog in progress (frames arriving are hours
    # old): the live snapshot above is the last LIVE frame, or empty.
    replay: bool = False
    replay_behind_s: float = 0.0
    replay_eta_s: Optional[float] = None
    # Live-first firmware: the panels ARE live during the replay; the backlog
    # only fills plots and logs. `replay_backlog_frames` is the queue depth
    # the last live frame reported.
    replay_live_panels: bool = False
    replay_backlog_frames: Optional[int] = None

    # -- derived -------------------------------------------------------------
    def flag(self, token: str) -> bool:
        return token in self.status_tokens

    @property
    def heaters_inhibited(self) -> bool:
        return "HEATER_INHIBITED" in self.status_tokens

    @property
    def overtemp_latched(self) -> bool:
        return "OVERTEMP_FAIL" in self.status_tokens

    def heater_temp_valid(self, heater: int) -> bool:
        """True when the sample that feeds `heater` has a live, finite
        reading -- the onboard refuses duty/target commands otherwise."""
        if not (0 <= heater < HEATER_COUNT):
            return False
        sample = HEATER_SAMPLE[heater]
        value = self.sample_temps[sample] if sample < len(self.sample_temps) else None
        return value is not None

    def motor(self, motor_id: int) -> MotorState:
        return self.motors[motor_id] if 0 <= motor_id < len(self.motors) else MotorState(motor_id)


def _finite(value: float, valid: bool) -> Optional[float]:
    return value if valid and isinstance(value, (int, float)) and math.isfinite(value) else None


def state_from_packet(pkt: TelemetryPacket, *, silence: bool = False,
                      link_age_s: Optional[float] = None) -> OnboardState:
    samples: List[Optional[float]] = []
    for i in range(SAMPLE_COUNT):
        if i < len(pkt.sample_temps_c):
            samples.append(_finite(pkt.sample_temps_c[i], pkt.sensor_valid.get(f"S{i}", True)))
        else:
            samples.append(None)
    resistance: List[Optional[float]] = []
    for i in range(SAMPLE_COUNT):
        value = pkt.sample_resistance_ohm[i] if i < len(pkt.sample_resistance_ohm) else None
        resistance.append(_finite(value, True) if value is not None else None)
    duties = [float(pkt.heater_duty[i]) if i < len(pkt.heater_duty) else 0.0
              for i in range(HEATER_COUNT)]
    by_motor = {int(s.get("motor_id", idx)): s for idx, s in enumerate(pkt.steppers)}
    motors: List[MotorState] = []
    for motor_id in range(MOTOR_COUNT):
        snap = by_motor.get(motor_id)
        if snap is None:
            motors.append(MotorState(motor_id))
            continue
        motors.append(MotorState(
            motor_id=motor_id, present=True,
            enabled=bool(snap.get("enabled")), zeroed=snap.get("zeroed"),
            moving=bool(snap.get("moving")), holding=bool(snap.get("holding")),
            healthy=bool(snap.get("healthy")),
            position=int(snap.get("position", 0)), target=int(snap.get("target", 0)),
            hz=float(snap.get("hz", 0.0)), microstep=int(snap.get("microstep", 0)),
            hold_s=float(snap.get("hold_s", 0.0)), pulses=int(snap.get("pulses", 0)),
            source=str(snap.get("source", "")), seq_name=str(snap.get("seq_name", "")),
            seq_state=str(snap.get("seq_state", "")),
        ))
    return OnboardState(
        have_packet=True, session_id=pkt.session_id, seq=pkt.seq,
        mode=(pkt.mode or "").upper(), phase=pkt.phase,
        status_tokens=frozenset(t for t in pkt.status.split("|") if t),
        component_state=dict(pkt.component_state),
        sample_temps=samples, sample_resistance=resistance, heater_duty=duties,
        ambient_temp_c=_finite(pkt.ambient_temp_c, pkt.sensor_valid.get("AT", True)),
        ambient_pressure_mbar=_finite(pkt.ambient_pressure_mbar, pkt.sensor_valid.get("AP", True)),
        uv=_finite(pkt.uv, pkt.sensor_valid.get("UV", True)),
        rtc_valid=bool(pkt.rtc_valid), motors=motors,
        fallback=pkt.fallback_active, link_loss_s=pkt.link_loss_s,
        energy_wh=pkt.energy_wh, budget_wh=pkt.budget_wh,
        budget_exhausted=pkt.budget_exhausted, heaters_active=pkt.heaters_active,
        queue_depth=pkt.queue_depth, plan_state=pkt.plan_state,
        silence=silence, link_age_s=link_age_s,
    )
