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
    healthy: bool = False
    moving: bool = False
    holding: bool = False
    hold_s: float = 0.0
    pulses: int = 0
    missed_deadlines: int = 0
    source: str = ""
    # Added 2026-08-28 (redesign spec §8). `zeroed` is None, and the two
    # sequence fields empty, when the onboard predates them -- the GUI
    # shows "unknown" rather than guessing.
    zeroed: Optional[bool] = None
    seq_name: str = ""
    seq_state: str = ""


@dataclass
class TelemetryPacket:
    session_id: str
    seq: int
    timestamp: str
    rtc_valid: int
    ambient_temp_c: float
    ambient_pressure_mbar: float
    uv: float
    sample_temps_c: List[float]
    heater_duty: List[float]
    # Compatibility resistance value per sample (`None` when the channel is
    # unmeasured — wire representation is a literal '-'). When the onboard
    # omits the RESISTANCE= segment entirely this stays an empty list.
    sample_resistance_ohm: List[Optional[float]]
    phase: str
    status: str
    mode: str = ""
    sensor_valid: Dict[str, bool] = field(default_factory=dict)
    sensor_age_ms: Dict[str, int] = field(default_factory=dict)
    component_state: Dict[str, str] = field(default_factory=dict)
    # Multi-motor snapshot list. `steppers[0]` = M0, `steppers[1]` = M1.
    # Length: 0 (no stepper segment), 1 (legacy single STEPPER=... segment),
    # or 2+ (new STEPPER0=/STEPPER1=... segments). Entries are dicts matching
    # the StepperSnapshot field set, with an extra 'motor_id' key.
    steppers: List[Dict] = field(default_factory=list)
    # Legacy accessor. Mirrors `steppers[0]` when present.
    stepper: Optional[StepperSnapshot] = None
    # `CTRL=` block (redesign spec §8): raw key -> value strings. Empty when
    # the onboard predates it; every typed accessor below then returns None.
    ctrl: Dict[str, str] = field(default_factory=dict)
    # `TX=<seconds>` is appended on the wire by the onboard drain (it is not
    # part of the stored frame): how old the frame was when it was sent.
    # 0-1 s means live; anything larger is the backlog being replayed. None
    # when the onboard predates the stamp.
    tx_age_s: Optional[float] = None

    # -- typed CTRL accessors ------------------------------------------------
    def _ctrl_bool(self, key: str) -> Optional[bool]:
        raw = self.ctrl.get(key)
        if raw is None:
            return None
        return raw not in ("0", "false", "False")

    def _ctrl_float(self, key: str) -> Optional[float]:
        raw = self.ctrl.get(key)
        if raw is None:
            return None
        try:
            return float(raw)
        except ValueError:
            return None

    def _ctrl_int(self, key: str) -> Optional[int]:
        raw = self.ctrl.get(key)
        if raw is None:
            return None
        try:
            return int(float(raw))
        except ValueError:
            return None

    @property
    def fallback_active(self) -> Optional[bool]:
        return self._ctrl_bool("fallback")

    @property
    def link_loss_s(self) -> Optional[float]:
        return self._ctrl_float("link_loss_s")

    @property
    def energy_wh(self) -> Optional[float]:
        return self._ctrl_float("energy_wh")

    @property
    def budget_wh(self) -> Optional[float]:
        return self._ctrl_float("budget_wh")

    @property
    def budget_exhausted(self) -> Optional[bool]:
        return self._ctrl_bool("budget_exhausted")

    @property
    def heaters_active(self) -> Optional[int]:
        return self._ctrl_int("heaters_active")

    @property
    def queue_depth(self) -> Optional[int]:
        return self._ctrl_int("queue")

    @property
    def plan_state(self) -> Optional[str]:
        return self.ctrl.get("plan")


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
            elif key == "ok":
                s.healthy = raw not in ("0", "false", "False")
            elif key == "mv":
                s.moving = raw not in ("0", "false", "False")
            elif key == "hold":
                s.holding = raw not in ("0", "false", "False")
            elif key == "hold_s":
                s.hold_s = float(raw)
            elif key == "pulses":
                s.pulses = int(raw)
            elif key == "missed":
                s.missed_deadlines = int(raw)
            elif key == "src":
                s.source = raw
            elif key == "zeroed":
                s.zeroed = raw not in ("0", "false", "False")
            elif key == "seq":
                s.seq_name = "" if raw == "-" else raw
            elif key == "seqst":
                s.seq_state = raw
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
        "healthy": snap.healthy,
        "moving": snap.moving,
        "holding": snap.holding,
        "hold_s": snap.hold_s,
        "pulses": snap.pulses,
        "missed_deadlines": snap.missed_deadlines,
        "source": snap.source,
        "zeroed": snap.zeroed,
        "seq_name": snap.seq_name,
        "seq_state": snap.seq_state,
    }


def parse_telemetry_csv(line: str) -> TelemetryPacket:
    parts = [p.strip() for p in line.strip().split(',')]
    # Fixed prefix = DATA + 7 scalar columns = 8 tokens before the
    # first sample. Plus at least HEATER_DUTY + PHASE + STATUS = 3 trailing
    # tokens. Minimum well-formed frame is 11 tokens.
    if len(parts) < 11:
        raise TelemetryParseError("telemetry packet too short")

    if parts[0] != "DATA":
        raise TelemetryParseError("missing DATA prefix")

    session_id = parts[1]
    seq = int(parts[2])
    timestamp = parts[3]
    rtc_valid = int(parts[4])
    ambient_temp_c = float(parts[5])
    ambient_pressure_mbar = float(parts[6])
    uv = float(parts[7])

    heater_field_index = None
    for idx, token in enumerate(parts):
        if token.startswith("HEATER_DUTY="):
            heater_field_index = idx
            break

    if heater_field_index is None:
        raise TelemetryParseError("missing HEATER_DUTY field")

    # Everything between the fixed prefix (index 8) and HEATER_DUTY= is a
    # sample temperature. Sample count is inferred — works for any N.
    sample_tokens = parts[8:heater_field_index]
    sample_temps_c = [float(x) for x in sample_tokens]

    heater_values_text = parts[heater_field_index].split('=', 1)[1]
    heater_duty = [float(x) for x in heater_values_text.split('|') if x != ""]

    phase = ""
    status = ""
    mode = ""
    sample_resistance_ohm: List[Optional[float]] = []
    legacy_stepper: Optional[StepperSnapshot] = None
    indexed_steppers: Dict[int, StepperSnapshot] = {}
    sensor_valid: Dict[str, bool] = {
        "AT": True, "AP": True, "UV": True,
        **{f"S{i}": True for i in range(len(sample_temps_c))},
    }
    sensor_age_ms: Dict[str, int] = {}
    component_state: Dict[str, str] = {}
    ctrl: Dict[str, str] = {}
    tx_age_s: Optional[float] = None
    for token in parts[heater_field_index + 1 :]:
        if token.startswith("PHASE="):
            phase = token.split('=', 1)[1]
        elif token.startswith("MODE="):
            mode = token.split('=', 1)[1]
        elif token.startswith("STATUS="):
            status = token.split('=', 1)[1]
        elif token.startswith("RESISTANCE="):
            # Pipe-separated compatibility resistance values, one per sample. A
            # literal '-' means the channel is unmeasured on this onboard.
            raw = token.split('=', 1)[1]
            if raw == "":
                sample_resistance_ohm = []
            else:
                for piece in raw.split('|'):
                    if piece == "" or piece == "-":
                        sample_resistance_ohm.append(None)
                    else:
                        try:
                            sample_resistance_ohm.append(float(piece))
                        except ValueError as exc:
                            raise TelemetryParseError(
                                f"invalid RESISTANCE value {piece!r}: {exc}"
                            ) from exc
        elif token.startswith("SENSOR_VALID="):
            sensor_valid = {}
            for piece in token.split("=", 1)[1].split("|"):
                if ":" not in piece:
                    continue
                key, raw = piece.split(":", 1)
                sensor_valid[key] = raw == "1"
        elif token.startswith("SENSOR_AGE_MS="):
            sensor_age_ms = {}
            for piece in token.split("=", 1)[1].split("|"):
                if ":" not in piece:
                    continue
                key, raw = piece.split(":", 1)
                try:
                    sensor_age_ms[key] = int(raw)
                except ValueError as exc:
                    raise TelemetryParseError(
                        f"invalid sensor age {piece!r}") from exc
        elif token.startswith("COMPONENT_STATE="):
            component_state = {}
            for piece in token.split("=", 1)[1].split("|"):
                if ":" not in piece:
                    continue
                key, state = piece.split(":", 1)
                component_state[key] = state
        elif token.startswith("CTRL="):
            ctrl = {}
            for piece in token.split("=", 1)[1].split("|"):
                if ":" not in piece:
                    continue
                key, value = piece.split(":", 1)
                ctrl[key] = value
        elif token.startswith("TX="):
            try:
                tx_age_s = max(0.0, float(token[3:]))
            except ValueError:
                tx_age_s = None
        elif token.startswith("STEPPER="):
            legacy_stepper = _parse_stepper_segment(token.split('=', 1)[1])
        elif token.startswith("STEPPER"):
            # STEPPER<digits>=... indexed dual-motor form.
            eq = token.find('=')
            if eq <= len("STEPPER"):
                continue
            suffix = token[len("STEPPER"):eq]
            if not suffix.isdigit():
                continue
            motor_id = int(suffix)
            indexed_steppers[motor_id] = _parse_stepper_segment(token[eq + 1:])

    if not phase or not status:
        raise TelemetryParseError("missing PHASE or STATUS field")

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
        uv=uv,
        sample_temps_c=sample_temps_c,
        heater_duty=heater_duty,
        sample_resistance_ohm=sample_resistance_ohm,
        phase=phase,
        status=status,
        mode=mode,
        sensor_valid=sensor_valid,
        sensor_age_ms=sensor_age_ms,
        component_state=component_state,
        steppers=steppers_list,
        stepper=primary_snapshot,
        ctrl=ctrl,
        tx_age_s=tx_age_s,
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
    "CHECK",
    "COMPONENTS",
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
    "HEATER_TEST",
    "SET_PID",
    "SET_TEMP_TARGET",
    "SET_ALL_TEMP_TARGETS",
    "CLEAR_TEMP_TARGET",
    "CLEAR_TEMP_TARGETS",
    "GET_THERMAL",
    "CLEAR_OVERRIDES",
    "SET_BENCH_MODE",
    "SET_TICK_HZ",
    "RADIO_SILENCE",
    "RADIO_RESUME",
    "SET_PHASE",
    "ARM",
    "DISARM",
    "ENTER_SAFE",
    "EXIT_SAFE",
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
    "SET_POSITION_ZERO",
    "BENDSEQ_LOAD",
    "BENDSEQ_RUN",
    "BENDSEQ_PAUSE",
    "BENDSEQ_RESUME",
    "BENDSEQ_STOP",
    "BENDSEQ_STATUS",
    "BENDSEQ_CLEAR",
    "PULL_ARM",
    "PULL_EXECUTE",
    "FALLBACK_PLAN",
    "FALLBACK_ARM",
    "FALLBACK_DISARM",
    "FALLBACK_STATUS",
    "MOTOR_DEBUG",
}


DEFAULT_COMMAND_TIMEOUT_S = 3.0

# Per-verb command timeout overrides, in seconds. Lives here (rather than in
# command_client.py or gui/dispatch.py) because both the CLI and the GUI
# dispatcher already import shared command semantics from this module
# (KNOWN_COMMANDS, build_command, parse_command_response) -- putting the
# timeout table anywhere else would mean picking one of the two callers to
# import from the other, or a third duplicate copy.
#
# Most commands ACK almost immediately: measured PING ~0.01s, COMPONENTS
# ~0.00s (both just format cached state -- see CommandType::kComponents in
# onboard/src/system_controller.cpp, which reads sensor_manager_ /
# stepper_ health flags with no I/O). CHECK is the one exception: it drives
# a real, synchronous hardware conversation -- DPS310/ADS1115 I2C probes, a
# full Sequent RTD Probe()+ReadAll() conversion, and two MAX31865 one-shot
# conversions (SensorManager::ActiveCheck in onboard/src/sensor_manager.cpp,
# called from CommandType::kCheck) -- and measured ~3.0s on a healthy Pi
# with NO hardware attached; real sensors add conversion time on top of
# that. The default 3.0s timeout races that exact duration, so CHECK (and
# every "CHECK <component>" variant -- CHECK SEQUENT_RTD, CHECK MAX31865,
# CHECK ALL, ...) gets a longer budget.
#
# Other candidates were checked and did NOT get an entry:
#   - SHUTDOWN_SAFE: sets in-memory override flags, then
#     StorageManager::FlushAndSync() -- a single fopen(ab)+fflush+fsync per
#     log path, no sensor/motor I/O. No evidence it needs more than the
#     default.
#   - COMPONENTS: reads cached health state only (see above); measured 0.00s.
#   - HEATER_TEST: validates args and sets a deferred override (the tick
#     thread applies it later); returns immediately, no hardware wait.
#   - BENDSEQ_LOAD/RUN/PAUSE/RESUME/STOP/STATUS/CLEAR: all just read/write
#     in-memory sequence state under a mutex; the actual stepper motion runs
#     asynchronously on the tick thread, not inline with the ACK.
COMMAND_TIMEOUTS: Dict[str, float] = {
    "CHECK": 15.0,
}


def timeout_for(command: str, default: float = DEFAULT_COMMAND_TIMEOUT_S) -> float:
    """Resolve the timeout budget (seconds) for a command by its first word.

    Lookup is by verb only, so "CHECK", "CHECK SEQUENT_RTD", and
    "CHECK MAX31865" all resolve to the same COMMAND_TIMEOUTS entry.
    Unknown/ordinary verbs (and an empty/blank command) fall back to
    `default`.
    """
    stripped = command.strip()
    if not stripped:
        return default
    verb = stripped.split()[0].upper()
    return COMMAND_TIMEOUTS.get(verb, default)


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

def validate_heater_index(idx: int, count: int = 6) -> Tuple[bool, str]:
    """Validate a heater index.

    Final-BOM heater channels: 0..5 (six sample-bank heaters, no box heater).
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


def validate_temperature_target(
    target_c: float, minimum_c: float = 0.0, maximum_c: float = 80.0
) -> Tuple[bool, str]:
    try:
        value = float(target_c)
    except (TypeError, ValueError):
        return False, "target must be numeric"
    if value < minimum_c or value > maximum_c:
        return False, f"target must be in [{minimum_c}, {maximum_c}] C"
    return True, f"{value:.3f}"


def validate_pid_gains(kp: float, ki: float, kd: float) -> Tuple[bool, str]:
    try:
        values = (float(kp), float(ki), float(kd))
    except (TypeError, ValueError):
        return False, "PID gains must be numeric"
    if any(value < 0.0 for value in values):
        return False, "PID gains must be non-negative"
    return True, " ".join(f"{value:.6g}" for value in values)


def validate_tick_hz(hz: float) -> Tuple[bool, str]:
    try:
        v = float(hz)
    except (TypeError, ValueError):
        return False, "hz must be numeric"
    if v < 0.1 or v > 5.0:
        return False, "hz must be in [0.1, 5.0]"
    return True, f"{v:.3f}"


def validate_speed_hz(hz: float, max_hz: float = 100.0) -> Tuple[bool, str]:
    """Validate a motor speed in full-step Hz.

    The onboard clamps `STEPPER_SET_SPEED` to `pull.max_step_hz` (100 Hz in
    the flight config) and NACKs `BENDSEQ_LOAD` speeds above it, so the
    ground station refuses anything above that bound up front instead of
    letting a 400 Hz request silently become 100 Hz.
    """
    try:
        v = float(hz)
    except (TypeError, ValueError):
        return False, "hz must be numeric"
    if v <= 0.0 or v > max_hz:
        return False, f"hz must be in (0, {max_hz}]"
    return True, f"{v:.3f}"


def validate_microstep(divisor: int) -> Tuple[bool, str]:
    if divisor not in (1, 2, 4, 8, 16, 32, 64, 128, 256):
        return False, "microstep must be a power of two from 1 through 256"
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
