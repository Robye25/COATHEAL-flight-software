"""One immutable snapshot of what the ground station currently knows about
the onboard, derived from the last telemetry packet plus ground-side
facts (radio silence, link age). Every panel reads this instead of
re-deriving its own view of the packet, and `gating.py` decides from it
which controls can succeed. No Qt here.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional, Tuple

from ..protocol import TelemetryPacket
from ..reply_format import parse_kv_body

MOTOR_COUNT = 2
HEATER_COUNT = 6
SAMPLE_COUNT = 8


@dataclass(frozen=True)
class Layout:
    """Which samples and heaters each motor group has, and the wiring behind
    them, as the onboard reports it (GET_LAYOUT; derived onboard from
    motor0.specimens / motor1.specimens). The defaults are the schematic
    frame: heater i reads sample i, motor 0 pulls S0-S3 and motor 1 S4-S7,
    and the two MAX31865 clicks read the first sample of each group. Firmware
    without GET_LAYOUT runs exactly that."""
    motor_samples: Tuple[Tuple[int, ...], ...] = ((0, 1, 2, 3), (4, 5, 6, 7))
    heater_samples: Tuple[int, ...] = (0, 1, 2, 3, 4, 5)   # heater h reads sample heater_samples[h]
    clicks: Tuple[int, ...] = (0, 4)                       # click n+1 reads sample clicks[n]
    rtd_channels: Tuple[int, ...] = (1, 2, 3, 4, 5, 6, 7, 8)
    heater_lines: Tuple[int, ...] = (19, 13, 6, 5, 24, 23)
    reported: bool = False                                  # the onboard answered GET_LAYOUT

    def sample_of_heater(self, heater: int) -> Optional[int]:
        return self.heater_samples[heater] if 0 <= heater < len(self.heater_samples) else None

    def heater_of_sample(self, sample: int) -> Optional[int]:
        return self.heater_samples.index(sample) if sample in self.heater_samples else None

    def motor_of_sample(self, sample: int) -> Optional[int]:
        return next((m for m, samples in enumerate(self.motor_samples) if sample in samples), None)

    def motor_of_heater(self, heater: int) -> Optional[int]:
        sample = self.sample_of_heater(heater)
        return None if sample is None else self.motor_of_sample(sample)

    def heaters_of_motor(self, motor: int) -> Tuple[int, ...]:
        """Heater indexes whose specimen the motor pulls, in sample order."""
        samples = self.motor_samples[motor] if 0 <= motor < len(self.motor_samples) else ()
        return tuple(h for s in samples for h in [self.heater_of_sample(s)] if h is not None)

    def click_of_sample(self, sample: int) -> Optional[int]:
        """1-based MAX31865 click number reading this sample, or None."""
        return self.clicks.index(sample) + 1 if sample in self.clicks else None

    def as_dict(self) -> Dict[str, object]:
        return {
            "motor_samples": [list(s) for s in self.motor_samples],
            "heater_samples": list(self.heater_samples), "clicks": list(self.clicks),
            "rtd_channels": list(self.rtd_channels), "heater_lines": list(self.heater_lines),
        }


DEFAULT_LAYOUT = Layout()


def parse_layout(body: str) -> Optional[Layout]:
    """A GET_LAYOUT reply body (`samples=8;heaters=6;motor0=0,1,2,3;...`), or
    None when it does not describe a usable layout."""
    kv = parse_kv_body(body)

    def numbers(key: str) -> Tuple[int, ...]:
        raw = kv.get(key, "")
        return tuple(int(piece) for piece in raw.split(",") if piece.strip()) if raw else ()

    try:
        samples = int(kv.get("samples", SAMPLE_COUNT))
        motors = tuple(numbers(f"motor{m}") for m in range(MOTOR_COUNT))
        layout = Layout(motor_samples=motors, heater_samples=numbers("heater_samples"),
                        clicks=numbers("clicks"), rtd_channels=numbers("rtd_channels"),
                        heater_lines=numbers("heater_lines"), reported=True)
    except ValueError:
        return None
    covered = sorted(s for group in motors for s in group)
    if (covered != list(range(samples)) or len(layout.heater_samples) != HEATER_COUNT
            or any(s not in covered for s in layout.heater_samples)
            or len(set(layout.heater_samples)) != len(layout.heater_samples)):
        return None
    return layout


def parse_lead_mm(body: str) -> Optional[float]:
    """The ball-screw lead (`lead_mm=<mm per revolution>`) a GET_LAYOUT reply
    carries, or None when the firmware does not report it (or reports
    something that is not a positive number)."""
    raw = parse_kv_body(body).get("lead_mm")
    try:
        value = float(raw) if raw is not None else math.nan
    except ValueError:
        return None
    return value if math.isfinite(value) and value > 0 else None


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
    missed: int = 0                    # missed step-pulse deadlines since boot
    source: str = ""
    seq_name: str = ""
    seq_state: str = ""
    # Drive-settings surface (2026-08-29). None: firmware predates it.
    amps: Optional[float] = None       # run current, A RMS
    accel: Optional[float] = None      # trapezoid slope, full-steps/s²
    mm: Optional[float] = None         # lead-derived linear position
    mm_tgt: Optional[float] = None
    # Driver die thermal state: "ok" / "warn" (>=~120 °C) / "hot"
    # (>=~150 °C, onboard safety disabled the motor). None: old firmware.
    thermal: Optional[str] = None


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
    # Bench debug arm (ARM_DEBUG) active onboard: unlocks open-loop duty on
    # channels without valid PT100 feedback. None: firmware predates the key.
    debug_armed: Optional[bool] = None
    # Active PID auto-tune channel ("H4"); None when idle or old firmware.
    tune_channel: Optional[str] = None
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
    # Which samples and heaters each motor group has (GET_LAYOUT).
    layout: Layout = DEFAULT_LAYOUT

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
        sample = self.layout.sample_of_heater(heater)
        if sample is None:
            return False
        value = self.sample_temps[sample] if sample < len(self.sample_temps) else None
        return value is not None

    def motor(self, motor_id: int) -> MotorState:
        return self.motors[motor_id] if 0 <= motor_id < len(self.motors) else MotorState(motor_id)


def _finite(value: float, valid: bool) -> Optional[float]:
    return value if valid and isinstance(value, (int, float)) and math.isfinite(value) else None


def state_from_packet(pkt: TelemetryPacket, *, silence: bool = False,
                      link_age_s: Optional[float] = None,
                      layout: Layout = DEFAULT_LAYOUT) -> OnboardState:
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
            missed=int(snap.get("missed_deadlines", 0) or 0),
            source=str(snap.get("source", "")), seq_name=str(snap.get("seq_name", "")),
            seq_state=str(snap.get("seq_state", "")),
            amps=snap.get("amps"), accel=snap.get("accel"),
            mm=snap.get("mm"), mm_tgt=snap.get("mm_tgt"),
            thermal=snap.get("thermal"),
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
        debug_armed=pkt.debug_armed,
        tune_channel=pkt.tune_channel,
        silence=silence, link_age_s=link_age_s, layout=layout,
    )
