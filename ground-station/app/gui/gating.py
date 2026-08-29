"""Why a control cannot succeed right now (redesign spec §6.2).

Every function returns None when the command may be sent, or a short
operator-facing reason. The onboard stays the authority: these only
prevent the NACKs the ground station can already predict from telemetry.
Unknown facts (no packet yet, firmware that does not report `zeroed`)
never block -- silence is the only gate that applies without telemetry.
"""
from __future__ import annotations

from typing import Optional

from .state import OnboardState

SILENCE = "radio silence active — send RADIO RESUME first"


def _mode_reason(state: OnboardState) -> Optional[str]:
    if not state.have_packet or not state.mode:
        return None
    if state.mode == "RUN":
        return None
    if state.mode == "SAFE":
        return "in SAFE mode — EXIT SAFE, then ARM"
    return "requires RUN mode — press ARM"


def arm_reason(state: OnboardState) -> Optional[str]:
    if state.silence:
        return SILENCE
    if state.have_packet and state.mode and state.mode != "STANDBY":
        return f"ARM requires STANDBY mode (now {state.mode})"
    return None


def disarm_reason(state: OnboardState) -> Optional[str]:
    if state.silence:
        return SILENCE
    if state.have_packet and state.mode and state.mode != "RUN":
        return f"DISARM requires RUN mode (now {state.mode})"
    return None


def exit_safe_reason(state: OnboardState) -> Optional[str]:
    if state.silence:
        return SILENCE
    if state.have_packet and state.mode and state.mode != "SAFE":
        return "not in SAFE mode"
    return None


def generic_reason(state: OnboardState) -> Optional[str]:
    """Commands with no precondition beyond the radio-silence gate."""
    return SILENCE if state.silence else None


def heater_reason(state: OnboardState, heater: int, *, needs_temperature: bool = True) -> Optional[str]:
    """Closed-loop heater commands (SET_TEMP_TARGET): the onboard requires
    valid PT100 feedback even while the bench debug arm is active."""
    if state.silence:
        return SILENCE
    reason = _mode_reason(state)
    if reason:
        return reason
    if needs_temperature and state.have_packet and not state.heater_temp_valid(heater):
        return f"S{heater} has no valid temperature — heater H{heater} cannot run"
    return None


def duty_reason(state: OnboardState, heater: int) -> Optional[str]:
    """Open-loop duty (SET_HEATER_DUTY): while the bench debug arm is
    active the onboard accepts duty without PT100 feedback, so the ground
    must not keep predicting a NACK that will not happen."""
    if state.debug_armed:
        return SILENCE if state.silence else _mode_reason(state)
    reason = heater_reason(state, heater)
    if reason and "no valid temperature" in reason:
        return reason + " — ARM_DEBUG in the console unlocks open-loop bench duty"
    return reason


def all_heaters_reason(state: OnboardState) -> Optional[str]:
    """SET_ALL_TEMP_TARGETS: every heated channel needs valid feedback,
    debug arm or not."""
    if state.silence:
        return SILENCE
    reason = _mode_reason(state)
    if reason:
        return reason
    if state.have_packet:
        invalid = [i for i in range(len(state.heater_duty)) if not state.heater_temp_valid(i)]
        if invalid:
            return "no valid temperature on S" + ", S".join(str(i) for i in invalid)
    return None


def all_duty_reason(state: OnboardState) -> Optional[str]:
    """SET_ALL_DUTY: open-loop, so the bench debug arm lifts the feedback
    requirement exactly as the onboard does."""
    if state.debug_armed:
        return SILENCE if state.silence else _mode_reason(state)
    reason = all_heaters_reason(state)
    if reason and reason.startswith("no valid temperature"):
        return reason + " — ARM_DEBUG in the console unlocks open-loop bench duty"
    return reason


def motion_reason(state: OnboardState, motor_id: int, *, needs_zero: bool,
                  needs_enable: bool = True) -> Optional[str]:
    if state.silence:
        return SILENCE
    reason = _mode_reason(state)
    if reason:
        return reason
    if state.fallback:
        return "link-loss fallback active onboard — manual motion is refused"
    if not state.have_packet:
        return None
    motor = state.motor(motor_id)
    if not motor.present:
        return None
    if needs_enable and not motor.enabled:
        return f"M{motor_id} not enabled — press ENABLE"
    if needs_zero and motor.zeroed is False:
        return f"M{motor_id} not zeroed — SET ZERO at the reference position first"
    return None


def enable_reason(state: OnboardState, motor_id: int) -> Optional[str]:
    if state.silence:
        return SILENCE
    reason = _mode_reason(state)
    if reason:
        return reason
    motor = state.motor(motor_id)
    if state.have_packet and motor.present and not motor.healthy:
        return f"M{motor_id} reports FAILED — check CHECK MOTOR{motor_id}"
    return None


def sequence_run_reason(state: OnboardState, motor_id: int) -> Optional[str]:
    reason = motion_reason(state, motor_id, needs_zero=True)
    if reason:
        return reason
    motor = state.motor(motor_id)
    if motor.seq_state == "run":
        return f"M{motor_id} already running sequence {motor.seq_name or ''}".rstrip()
    return None
