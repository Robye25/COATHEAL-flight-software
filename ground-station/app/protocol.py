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
