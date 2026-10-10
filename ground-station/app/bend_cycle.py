"""The bend cycle the Motion tab memorises and plays (owner request
2026-10-10): drive the specimen to a positive limit, soak there, drive it
to a negative limit, soak there, for a number of cycles, then come back to
zero. This module is the pure part: the `BENDSEQ_LOAD` line the onboard
runs, its duration estimate, and the per-motor settings round trip. No Qt.

Speed and acceleration are deliberately absent: they are the motor's own
(`STEPPER_SET_SPEED` / `STEPPER_SET_ACCEL`, Motion tab "Drive settings"),
and the onboard refuses a per-step speed since 2026-10-10.
"""
from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from typing import Any, Dict, Mapping, Optional

from .protocol import FULL_STEPS_PER_MM, validate_move_mm

# Mirror the onboard: kMaxSequenceRepeat (bend_sequence.hpp), kMaxHoldSeconds
# and IsSequenceNameValid (system_controller.cpp).
MAX_CYCLES = 1000
MAX_SOAK_S = 86400.0
MAX_NAME_LEN = 32


@dataclass(frozen=True)
class BendCycle:
    name: str = "cycle"
    plus_mm: float = 2.0          # the positive limit (>= 0)
    minus_mm: float = 0.0         # the negative limit (<= 0), signed
    cycles: int = 1
    upper_soak_s: float = 5.0     # hold at the positive limit
    lower_soak_s: float = 5.0     # hold at the negative limit
    return_to_zero: bool = True   # one last step to 0 after the cycles


FIELDS = tuple(BendCycle.__dataclass_fields__)


def validate(cycle: BendCycle) -> Optional[str]:
    """None when the cycle can be sent, else the operator-facing reason."""
    name = cycle.name.strip()
    if not name or len(name) > MAX_NAME_LEN or not all(c.isalnum() or c in "_-" for c in name):
        return f"name: letters, digits, _ or -, up to {MAX_NAME_LEN} characters"
    for label, mm in (("+ limit", cycle.plus_mm), ("− limit", cycle.minus_mm)):
        ok, msg = validate_move_mm(mm)
        if not ok:
            return f"{label}: {msg}"
    if cycle.plus_mm < 0.0:
        return "+ limit must be zero or positive"
    if cycle.minus_mm > 0.0:
        return "− limit must be zero or negative"
    if cycle.plus_mm == cycle.minus_mm:
        return "the two limits are equal — nothing to bend"
    if not isinstance(cycle.cycles, int) or not 1 <= cycle.cycles <= MAX_CYCLES:
        return f"cycles must be 1..{MAX_CYCLES}"
    for label, soak in (("soak at +", cycle.upper_soak_s), ("soak at −", cycle.lower_soak_s)):
        if not math.isfinite(soak) or soak < 0.0 or soak > MAX_SOAK_S:
            return f"{label}: must be 0..{MAX_SOAK_S:g} s"
    return None


def usteps(mm: float, microstep: int) -> int:
    """mm of ball-screw travel as absolute microsteps at `microstep`."""
    return int(round(mm * FULL_STEPS_PER_MM * max(1, int(microstep))))


def load_command(motor_id: int, cycle: BendCycle, microstep: int) -> str:
    """`BENDSEQ_LOAD <motor> <name> <+limit>:<soak> <−limit>:<soak> [repeat=<n>] [0:0]`
    with the limits as absolute microsteps at the motor's live divisor
    (never a guess: a cycle encoded at µ4 for a motor on µ16 bends four
    times short). `repeat=` is left out for a single cycle and the final
    `0:0` when the negative limit already is zero."""
    steps = [f"{usteps(cycle.plus_mm, microstep)}:{cycle.upper_soak_s:g}",
             f"{usteps(cycle.minus_mm, microstep)}:{cycle.lower_soak_s:g}"]
    if cycle.cycles > 1:
        steps.append(f"repeat={cycle.cycles}")
    if cycle.return_to_zero and usteps(cycle.minus_mm, microstep) != 0:
        steps.append("0:0")
    return f"BENDSEQ_LOAD {motor_id} {cycle.name.strip()} {' '.join(steps)}"


def duration_s(cycle: BendCycle, speed_mm_s: Optional[float]) -> Optional[float]:
    """Travel at `speed_mm_s` plus the soaks, from a motor standing at zero.
    Acceleration ramps are ignored; None without a usable speed."""
    if speed_mm_s is None or not speed_mm_s > 0.0:
        return None
    span = cycle.plus_mm - cycle.minus_mm
    travel = cycle.plus_mm + span + 2.0 * span * (cycle.cycles - 1)
    if cycle.return_to_zero:
        travel += abs(cycle.minus_mm)
    return travel / speed_mm_s + cycle.cycles * (cycle.upper_soak_s + cycle.lower_soak_s)


def format_duration(seconds: float) -> str:
    total = int(round(seconds))
    if total < 60:
        return f"{total} s"
    if total < 3600:
        return f"{total // 60} min {total % 60:02d} s"
    return f"{total // 3600} h {(total % 3600) // 60:02d} min"


def describe(cycle: BendCycle, speed_mm_s: Optional[float]) -> str:
    """One line for the operator: what the cycle does and how long it takes."""
    text = (f"{cycle.cycles} × (+{cycle.plus_mm:.3f} mm soak {cycle.upper_soak_s:g} s → "
            f"{cycle.minus_mm:.3f} mm soak {cycle.lower_soak_s:g} s)")
    if cycle.return_to_zero and cycle.minus_mm != 0.0:
        text += ", then back to 0"
    estimate = duration_s(cycle, speed_mm_s)
    if estimate is not None:
        text += f" · ≈ {format_duration(estimate)} at {speed_mm_s:.2f} mm/s"
    return text


def progress_text(status_body: str) -> Optional[str]:
    """`cycle 3/10 · step 5/21 · running` from a BENDSEQ_STATUS reply body
    (`motor=0;zeroed=1;running=1;paused=0;name=cycle;step=4;total=21;cycle=3;cycles=10`),
    `idle` when nothing is active, None for a body this cannot read."""
    fields: Dict[str, str] = {}
    for item in status_body.split(";"):
        key, sep, value = item.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    if "running" not in fields:
        return None
    if "total" not in fields or not fields.get("name"):
        return "idle"
    try:
        step = int(fields.get("step", "0")) + 1
        total = int(fields["total"])
        cycle = int(fields.get("cycle", "1"))
        cycles = int(fields.get("cycles", "1"))
    except ValueError:
        return None
    state = "paused" if fields.get("paused") == "1" else ("running" if fields.get("running") == "1" else "idle")
    text = f"{fields['name']}: cycle {cycle}/{cycles} · step {min(step, total)}/{total} · {state}"
    if fields.get("fault"):
        text += f" · fault: {fields['fault']}"
    return text


# -- settings round trip (QSettings hands back strings on some platforms) ------
def to_mapping(cycle: BendCycle) -> Dict[str, Any]:
    return asdict(cycle)


def _as_bool(value: Any, default: bool) -> bool:
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    return default


def from_mapping(values: Mapping[str, Any], default: BendCycle = BendCycle()) -> BendCycle:
    """A cycle from saved values; anything missing or unreadable keeps the
    default's value, so a stale or hand-edited setting cannot stop the tab."""
    def pick(field: str, convert):
        if field not in values or values[field] is None:
            return getattr(default, field)
        try:
            return convert(values[field])
        except (TypeError, ValueError):
            return getattr(default, field)

    cycles = pick("cycles", lambda v: int(float(v)))
    return BendCycle(
        name=str(pick("name", str)).strip() or default.name,
        plus_mm=pick("plus_mm", float),
        minus_mm=pick("minus_mm", float),
        cycles=cycles if 1 <= cycles <= MAX_CYCLES else default.cycles,
        upper_soak_s=pick("upper_soak_s", float),
        lower_soak_s=pick("lower_soak_s", float),
        return_to_zero=_as_bool(values.get("return_to_zero"), default.return_to_zero),
    )
