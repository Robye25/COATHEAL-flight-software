"""Motion truth from `MOTOR_DEBUG` replies (no Qt).

`MotorDebugSample` parses one reply; `MotionEstimator` turns a stream of
samples into what the operator wants to know: is the sequencer stepping
(ΔMSCNT), is the ramp generator moving (ΔXACTUAL, VACTUAL), how fast in
full-steps/s, rev/s and mm/s, and a one-line verdict.

MSCNT is the 10-bit microstep sine-table index: 256 counts per full step,
independent of the MRES setting, so |ΔMSCNT| / 256 is full steps regardless
of microstepping. XACTUAL counts 1/256 full steps.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional, Tuple

from ..reply_format import parse_kv_body

FULL_STEPS_PER_REV = 200
MSCNT_PER_FULL_STEP = 256
XACTUAL_PER_FULL_STEP = 256
MSCNT_MODULUS = 1024
DEFAULT_MM_PER_REV = 1.5
WINDOW_S = 3.0


def _int(kv: Dict[str, str], key: str) -> Optional[int]:
    raw = kv.get(key)
    if raw is None:
        return None
    try:
        return int(raw, 0)
    except ValueError:
        try:
            return int(float(raw))
        except ValueError:
            return None


def _flag(kv: Dict[str, str], key: str) -> Optional[bool]:
    v = _int(kv, key)
    return None if v is None else bool(v)


@dataclass(frozen=True)
class MotorDebugSample:
    t: float
    motor: int
    sw_pos: Optional[int]
    sw_tgt: Optional[int]
    sw_hz: Optional[float]
    usteps: Optional[int]
    enabled: Optional[bool]
    moving: Optional[bool]
    xactual: Optional[int]
    xtarget: Optional[int]
    vactual: Optional[int]
    mscnt: Optional[int]
    tstep: Optional[int]
    stst: Optional[bool]
    cs_actual: Optional[int]
    sg_result: Optional[int]
    drv_enn: Optional[bool]
    sd_mode: Optional[bool]
    toff: Optional[int]
    faults: Tuple[str, ...]
    raw: Dict[str, str] = field(default_factory=dict)

    @classmethod
    def parse(cls, body: str, t: float) -> "MotorDebugSample":
        kv = parse_kv_body(body)
        faults = tuple(name for name in ("ot", "otpw", "s2ga", "s2gb", "s2vsa", "s2vsb", "ola", "olb")
                       if _int(kv, name) == 1)
        hz = kv.get("sw_hz")
        try:
            sw_hz = float(hz) if hz is not None else None
        except ValueError:
            sw_hz = None
        return cls(
            t=t, motor=_int(kv, "motor") or 0,
            sw_pos=_int(kv, "sw_pos"), sw_tgt=_int(kv, "sw_tgt"), sw_hz=sw_hz,
            usteps=_int(kv, "usteps") or _int(kv, "us"),
            enabled=_flag(kv, "enabled"), moving=_flag(kv, "moving"),
            xactual=_int(kv, "xactual"), xtarget=_int(kv, "xtarget"),
            vactual=_int(kv, "vactual"), mscnt=_int(kv, "mscnt"), tstep=_int(kv, "tstep"),
            stst=_flag(kv, "stst"), cs_actual=_int(kv, "cs_actual"), sg_result=_int(kv, "sg_result"),
            drv_enn=_flag(kv, "drv_enn"), sd_mode=_flag(kv, "sd_mode"), toff=_int(kv, "toff"),
            faults=faults, raw=kv,
        )


def mscnt_delta(previous: int, current: int) -> int:
    """Signed minimal change of the 10-bit sine-table index."""
    d = (current - previous) % MSCNT_MODULUS
    return d - MSCNT_MODULUS if d > MSCNT_MODULUS // 2 else d


@dataclass(frozen=True)
class MotionEstimate:
    sequencer_full_steps_s: float      # from |ΔMSCNT|, the coil-driving truth
    ramp_full_steps_s: float           # from ΔXACTUAL (signed)
    rev_s: float
    mm_s: float
    travel_full_steps: float           # accumulated |ΔMSCNT| since start
    travel_mm: float
    samples: int
    verdict: str
    color: str                         # "green" | "amber" | "red" | "gray"


class MotionEstimator:
    def __init__(self, mm_per_rev: float = DEFAULT_MM_PER_REV, window_s: float = WINDOW_S):
        self.mm_per_rev = float(mm_per_rev)
        self.window_s = float(window_s)
        self._samples: Deque[MotorDebugSample] = deque()
        self._travel_counts = 0.0
        self._last: Optional[MotorDebugSample] = None

    def reset(self) -> None:
        self._samples.clear()
        self._travel_counts = 0.0
        self._last = None

    def add(self, sample: MotorDebugSample) -> MotionEstimate:
        if self._last is not None and self._last.mscnt is not None and sample.mscnt is not None:
            self._travel_counts += abs(mscnt_delta(self._last.mscnt, sample.mscnt))
        self._last = sample
        self._samples.append(sample)
        while self._samples and (sample.t - self._samples[0].t) > self.window_s:
            self._samples.popleft()
        return self.estimate()

    def estimate(self) -> MotionEstimate:
        s = list(self._samples)
        seq_rate = ramp_rate = 0.0
        if len(s) >= 2:
            dt = s[-1].t - s[0].t
            if dt > 0:
                counts = 0.0
                for a, b in zip(s, s[1:]):
                    if a.mscnt is not None and b.mscnt is not None:
                        counts += abs(mscnt_delta(a.mscnt, b.mscnt))
                seq_rate = counts / MSCNT_PER_FULL_STEP / dt
                if s[0].xactual is not None and s[-1].xactual is not None:
                    ramp_rate = (s[-1].xactual - s[0].xactual) / XACTUAL_PER_FULL_STEP / dt
        rev_s = seq_rate / FULL_STEPS_PER_REV
        travel_fs = self._travel_counts / MSCNT_PER_FULL_STEP
        verdict, color = self._verdict(s[-1] if s else None, seq_rate, ramp_rate)
        return MotionEstimate(seq_rate, ramp_rate, rev_s, rev_s * self.mm_per_rev,
                              travel_fs, travel_fs / FULL_STEPS_PER_REV * self.mm_per_rev,
                              len(s), verdict, color)

    def _verdict(self, last: Optional[MotorDebugSample], seq_rate: float, ramp_rate: float) -> Tuple[str, str]:
        if last is None:
            return "no sample yet", "gray"
        if last.faults:
            return "DRIVER FAULT: " + " ".join(last.faults) + " (open-load flags are only valid at standstill)", "red"
        if last.sd_mode:
            return "SD_MODE=1: module strapped for STEP/DIR — SPI motion can never move it", "red"
        power_off = bool(last.drv_enn) or last.toff == 0
        sequencer_moving = seq_rate > 0.5
        ramp_moving = abs(ramp_rate) > 0.5 or (last.vactual not in (None, 0))
        if sequencer_moving and not power_off:
            return f"MOVING — sequencer stepping at {seq_rate:.1f} full-steps/s", "green"
        if sequencer_moving and power_off:
            return "sequencer stepping but the power stage is OFF (DRV_ENN=1 or TOFF=0): coils are not driven", "red"
        if ramp_moving and not sequencer_moving:
            return "COMMANDED BUT NOT STEPPING — ramp generator moves, MSCNT frozen (sequencer not driving the coils)", "red"
        if last.moving:
            return "firmware says moving but the chip is at standstill — check DRV_ENN / TOFF / SD_MODE", "red"
        if power_off:
            return "STANDSTILL — power stage off (motor disabled)", "gray"
        return "STANDSTILL — enabled, holding current" if last.enabled else "STANDSTILL", "gray"
