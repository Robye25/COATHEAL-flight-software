from __future__ import annotations

from dataclasses import dataclass
from typing import List


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


class TelemetryParseError(ValueError):
    pass


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
    for token in parts[heater_field_index + 1 :]:
        if token.startswith("PHASE="):
            phase = token.split('=', 1)[1]
        elif token.startswith("STATUS="):
            status = token.split('=', 1)[1]

    if not phase or not status:
        raise TelemetryParseError("missing PHASE or STATUS field")

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
    )


def build_ack(session_id: str, seq: int) -> str:
    return f"ACK,{session_id},{seq}\n"


def build_command(command: str) -> str:
    command = command.strip()
    if not command:
        raise ValueError("empty command")
    return command + "\n"


# Commands accepted by the onboard command server. Kept in sync with
# onboard/src/command_parser.cpp.
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
