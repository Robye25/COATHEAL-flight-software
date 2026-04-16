from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple


@dataclass
class StepperSnapshot:
    position: int = 0
    target: int = 0
    hz: float = 0.0
    microstep: int = 1
    enabled: bool = False
    moving: bool = False
    holding: bool = False
    hold_s: float = 0.0
    pulses: int = 0
    source: str = ""


@dataclass
class TelemetryPacket:
    session_id: str
    seq: int
    timestamp: str
    rtc_valid: int
    ambient_temp_c: float
    ambient_pressure_mbar: float
    ambient_humidity_pct: float
    uv: float
    box_temp_c: float
    sample_temps_c: List[float]
    heater_duty: List[float]
    phase: str
    status: str
    mode: str = ""
    # Rev-B: multi-motor snapshot list. `steppers[0]` = M0, `steppers[1]` = M1.
    # Length: 0 (legacy log with no stepper segment), 1 (legacy single
    # STEPPER=... segment), or 2+ (new STEPPER0=/STEPPER1=... segments).
    # Entries are dicts matching the StepperSnapshot field set, with an
    # extra 'motor_id' key carrying the index.
    steppers: List[Dict] = field(default_factory=list)
    # Legacy accessor. Mirrors `steppers[0]` when present so existing callers
    # (`pkt.stepper.position`, etc.) keep working.
    stepper: Optional[StepperSnapshot] = None


class TelemetryParseError(ValueError):
    pass


def _parse_stepper_segment(value: str) -> StepperSnapshot:
    s = StepperSnapshot()
    for piece in value.split("|"):
        if not piece:
            continue
        if ":" not in piece:
            raise TelemetryParseError(f"malformed STEPPER pair: {piece!r}")
        key, raw = piece.split(":", 1)
        try:
            if key == "pos":
                s.position = int(raw)
            elif key == "tgt":
                s.target = int(raw)
            elif key == "hz":
                s.hz = float(raw)
            elif key == "us":
                s.microstep = int(raw)
            elif key == "en":
                s.enabled = raw not in ("0", "false", "False")
            elif key == "mv":
                s.moving = raw not in ("0", "false", "False")
            elif key == "hold":
                s.holding = raw not in ("0", "false", "False")
            elif key == "hold_s":
                s.hold_s = float(raw)
            elif key == "pulses":
                s.pulses = int(raw)
            elif key == "src":
                s.source = raw
            # unknown keys silently ignored (forward-compat)
        except ValueError as exc:
            raise TelemetryParseError(f"invalid STEPPER {key}={raw!r}: {exc}") from exc
    return s


def _snapshot_to_dict(snap: StepperSnapshot, motor_id: int) -> Dict:
    return {
        "motor_id": motor_id,
        "position": snap.position,
        "target": snap.target,
        "hz": snap.hz,
        "microstep": snap.microstep,
        "enabled": snap.enabled,
        "moving": snap.moving,
        "holding": snap.holding,
        "hold_s": snap.hold_s,
        "pulses": snap.pulses,
        "source": snap.source,
    }


def parse_telemetry_csv(line: str) -> TelemetryPacket:
    parts = [p.strip() for p in line.strip().split(',')]
    if len(parts) < 13:
        raise TelemetryParseError("telemetry packet too short")

    if parts[0] != "DATA":
        raise TelemetryParseError("missing DATA prefix")

    session_id = parts[1]
    seq = int(parts[2])
    timestamp = parts[3]
    rtc_valid = int(parts[4])
    ambient_temp_c = float(parts[5])
    ambient_pressure_mbar = float(parts[6])
    ambient_humidity_pct = float(parts[7])
    uv = float(parts[8])
    box_temp_c = float(parts[9])

    heater_field_index = None
    for idx, token in enumerate(parts):
        if token.startswith("HEATER_DUTY="):
            heater_field_index = idx
            break

    if heater_field_index is None:
        raise TelemetryParseError("missing HEATER_DUTY field")

    sample_tokens = parts[10:heater_field_index]
    sample_temps_c = [float(x) for x in sample_tokens]

    heater_values_text = parts[heater_field_index].split('=', 1)[1]
    heater_duty = [float(x) for x in heater_values_text.split('|') if x != ""]

    phase = ""
    status = ""
    mode = ""
    # Collect both old-style single STEPPER= and new-style STEPPERn=
    # segments. Indexed entries win over the legacy unindexed segment if
    # both are somehow present (shouldn't happen, but the behaviour is
    # deterministic: new log schema always takes precedence).
    legacy_stepper: Optional[StepperSnapshot] = None
    indexed_steppers: Dict[int, StepperSnapshot] = {}
    for token in parts[heater_field_index + 1 :]:
        if token.startswith("PHASE="):
            phase = token.split('=', 1)[1]
        elif token.startswith("MODE="):
            mode = token.split('=', 1)[1]
        elif token.startswith("STATUS="):
            status = token.split('=', 1)[1]
        elif token.startswith("STEPPER="):
            legacy_stepper = _parse_stepper_segment(token.split('=', 1)[1])
        elif token.startswith("STEPPER"):
            # STEPPER<digits>=...  — Rev-B dual-motor form. Anything after
            # STEPPER up to the '=' is a non-negative integer motor id.
            eq = token.find('=')
            if eq <= len("STEPPER"):
                # Not a well-formed STEPPER<n>= segment; drop silently for
                # forward-compat.
                continue
            suffix = token[len("STEPPER"):eq]
            if not suffix.isdigit():
                # Unknown variant (e.g. STEPPERX=...) — ignore.
                continue
            motor_id = int(suffix)
            indexed_steppers[motor_id] = _parse_stepper_segment(token[eq + 1:])

    if not phase or not status:
        raise TelemetryParseError("missing PHASE or STATUS field")

    # Build ordered snapshot list. Prefer indexed entries; otherwise fall
    # back to the single legacy STEPPER= segment as motor_id=0.
    steppers_list: List[Dict] = []
    primary_snapshot: Optional[StepperSnapshot] = None
    if indexed_steppers:
        for mid in sorted(indexed_steppers.keys()):
            snap = indexed_steppers[mid]
            steppers_list.append(_snapshot_to_dict(snap, mid))
            if primary_snapshot is None:
                primary_snapshot = snap
    elif legacy_stepper is not None:
        steppers_list.append(_snapshot_to_dict(legacy_stepper, 0))
        primary_snapshot = legacy_stepper

    return TelemetryPacket(
        session_id=session_id,
        seq=seq,
        timestamp=timestamp,
        rtc_valid=rtc_valid,
        ambient_temp_c=ambient_temp_c,
        ambient_pressure_mbar=ambient_pressure_mbar,
        ambient_humidity_pct=ambient_humidity_pct,
        uv=uv,
        box_temp_c=box_temp_c,
        sample_temps_c=sample_temps_c,
        heater_duty=heater_duty,
        phase=phase,
        status=status,
        mode=mode,
        steppers=steppers_list,
        stepper=primary_snapshot,
    )


def build_ack(session_id: str, seq: int) -> str:
    return f"ACK,{session_id},{seq}\n"


def build_command(command: str) -> str:
    command = command.strip()
    if not command:
        raise ValueError("empty command")
    return command + "\n"


KNOWN_COMMANDS = {
    "PING",
    "STATUS",
    "FORCE_START",
    "FORCE_STOP",
    "ON",
    "OFF",
    "HEATERS_OFF",
    "RESET_CTRL",
    "RESET",
    "SHUTDOWN_SAFE",
    "ARM_DEBUG",
    "DISARM_DEBUG",
    "SET_HEATER_DUTY",
    "SET_ALL_DUTY",
    "SET_PID",
    "CLEAR_OVERRIDES",
    "SET_BENCH_MODE",
    "SET_TICK_HZ",
    "RADIO_SILENCE",
    "RADIO_RESUME",
    "ARM",
    "DISARM",
    "ENTER_SAFE",
    "EXIT_SAFE",
    "SECONDARY_CYCLE",
    "STEPPER_MOVE",
    "STEPPER_MOVETO",
    "STEPPER_ROTATE",
    "STEPPER_HOME",
    "STEPPER_STOP",
    "STEPPER_SET_SPEED",
    "STEPPER_SET_MICROSTEP",
    "STEPPER_ENABLE",
    "STEPPER_DISABLE",
    "STEPPER_BEND",
}


@dataclass
class HeatingCycleEvent:
    session_id: str
    cycle_id: int
    start_ts: str
    peak_temp_c: float
    hold_duration_s: float
    cooldown_rate_c_per_s: float
    specimen_index: int


def parse_heating_cycle_event(line: str) -> HeatingCycleEvent:
    """Parse an `EVT,CYCLE,...` line emitted by the onboard."""
    parts = [p.strip() for p in line.strip().split(",")]
    if len(parts) < 9 or parts[0] != "EVT" or parts[1] != "CYCLE":
        raise TelemetryParseError("not an EVT,CYCLE frame")
    try:
        return HeatingCycleEvent(
            session_id=parts[2],
            cycle_id=int(parts[3]),
            start_ts=parts[4],
            peak_temp_c=float(parts[5]),
            hold_duration_s=float(parts[6]),
            cooldown_rate_c_per_s=float(parts[7]),
            specimen_index=int(parts[8]),
        )
    except ValueError as exc:
        raise TelemetryParseError(f"invalid EVT,CYCLE fields: {exc}") from exc


@dataclass
class PullEvent:
    """One bend-and-hold cycle completed by a motor.

    Wire format (newline-terminated):
        EVT,PULL,<session>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,
            <hold_s>,<samples>
    where <samples> is pipe-separated specimen indices (e.g. ``0|1|2|3``)
    or ``-`` for no specimens.
    """

    session_id: str
    pull_id: int
    motor_id: int
    start_ts: str
    steps_moved: int
    hold_s: float
    samples: List[int] = field(default_factory=list)


def parse_pull_event(line: str) -> PullEvent:
    """Parse an `EVT,PULL,...` line emitted by the stepper subsystem.

    Accepts a trailing ``-`` or empty string in the samples field (meaning
    no specimens recorded). Raises ``TelemetryParseError`` on any other
    malformed input.
    """
    parts = [p.strip() for p in line.strip().split(",")]
    if len(parts) < 9 or parts[0] != "EVT" or parts[1] != "PULL":
        raise TelemetryParseError("not an EVT,PULL frame")
    samples_raw = parts[8]
    samples: List[int] = []
    if samples_raw and samples_raw != "-":
        try:
            samples = [int(x) for x in samples_raw.split("|") if x != ""]
        except ValueError as exc:
            raise TelemetryParseError(f"invalid EVT,PULL samples: {exc}") from exc
    try:
        return PullEvent(
            session_id=parts[2],
            pull_id=int(parts[3]),
            motor_id=int(parts[4]),
            start_ts=parts[5],
            steps_moved=int(parts[6]),
            hold_s=float(parts[7]),
            samples=samples,
        )
    except ValueError as exc:
        raise TelemetryParseError(f"invalid EVT,PULL fields: {exc}") from exc


@dataclass
class CommandResponse:
    ok: bool
    command: str
    body: str = ""
    error: str = ""
    raw: str = ""


def parse_command_response(line: str) -> CommandResponse:
    """Parse the onboard reply format: `ACK,<cmd>,<body>` or `NACK,<cmd>,<reason>`.

    `command_parser.cpp` actually emits `ACK,<cmd>,<message>` without a
    session/seq echo today, so we accept that shape. Unknown shapes return
    ok=False with the raw line as the body so the operator sees something.
    """
    raw = line.strip()
    if not raw:
        return CommandResponse(ok=False, command="", error="empty response", raw=raw)
    parts = raw.split(",", 2)
    tag = parts[0].upper()
    if tag == "ACK" and len(parts) >= 2:
        return CommandResponse(ok=True, command=parts[1],
                               body=parts[2] if len(parts) >= 3 else "", raw=raw)
    if tag == "NACK" and len(parts) >= 2:
        return CommandResponse(ok=False, command=parts[1],
                               error=parts[2] if len(parts) >= 3 else "", raw=raw)
    return CommandResponse(ok=False, command="", error="unrecognised reply", raw=raw)


# --- Argument validators ------------------------------------------------------
# Each returns (ok, normalised_or_error). Used by GUI before enabling Send and
# by CLI before wire-encoding. Single source of truth for bounds.

def validate_heater_index(idx: int, count: int = 9) -> Tuple[bool, str]:
    """Validate a heater index.

    Rev-B heater channels: 0..7 = sample heaters, 8 = electronics BOX
    heater. Total count = 9. Existing call sites that passed ``count=10``
    (legacy 9 samples + box) keep working because they override explicitly.
    """
    if not isinstance(idx, int) or idx < 0 or idx >= count:
        return False, f"index must be in [0, {count - 1}]"
    return True, str(idx)


def validate_duty(duty: float) -> Tuple[bool, str]:
    try:
        d = float(duty)
    except (TypeError, ValueError):
        return False, "duty must be numeric"
    if d < 0.0 or d > 1.0:
        return False, "duty must be in [0.0, 1.0]"
    return True, f"{d:.3f}"


def validate_tick_hz(hz: float) -> Tuple[bool, str]:
    try:
        v = float(hz)
    except (TypeError, ValueError):
        return False, "hz must be numeric"
    if v < 0.1 or v > 5.0:
        return False, "hz must be in [0.1, 5.0]"
    return True, f"{v:.3f}"


def validate_speed_hz(hz: float, max_hz: float = 5000.0) -> Tuple[bool, str]:
    try:
        v = float(hz)
    except (TypeError, ValueError):
        return False, "hz must be numeric"
    if v <= 0.0 or v > max_hz:
        return False, f"hz must be in (0, {max_hz}]"
    return True, f"{v:.3f}"


def validate_microstep(divisor: int) -> Tuple[bool, str]:
    if divisor not in (1, 2, 4, 8, 16, 32):
        return False, "microstep must be 1, 2, 4, 8, 16 or 32"
    return True, str(divisor)


def validate_stepper_move(steps: int, max_range: int = 200000) -> Tuple[bool, str]:
    try:
        n = int(steps)
    except (TypeError, ValueError):
        return False, "steps must be integer"
    if abs(n) > max_range:
        return False, f"steps exceed max_range {max_range}"
    return True, str(n)


def validate_revolutions(revs: float) -> Tuple[bool, str]:
    try:
        r = float(revs)
    except (TypeError, ValueError):
        return False, "revs must be numeric"
    if abs(r) > 1e6:
        return False, "revs unrealistically large"
    return True, f"{r:.4f}"
