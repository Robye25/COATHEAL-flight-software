"""Alarm model for the alarm strip (redesign spec §5.2). Pure: evaluated
from an `OnboardState`; acknowledgement state lives here so the strip is
a dumb renderer.

An acknowledged alarm stays listed (dimmed) until its condition clears; if
the condition returns after clearing it is raised again as new.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

from .series_store import format_elapsed
from .state import OnboardState

RED = "red"
AMBER = "amber"

# Components whose non-OK state raises the SENSOR alarm, and the wire
# flags that do the same.
_SENSOR_COMPONENTS = ("DPS310", "ADS1115", "SEQUENT_RTD", "PWM")
_SENSOR_FLAGS = ("RESISTANCE_FAIL", "SPI_FAIL", "I2C_FAIL", "SD_FAIL", "USB_FAIL", "PWM_FAIL")
_COMPONENT_BAD = {"DEGRADED", "STALE", "FAILED"}
QUEUE_ALARM_FRAMES = 100
LINK_STALE_S = 5.0


@dataclass(frozen=True)
class Alarm:
    key: str
    text: str
    severity: str = RED
    acked: bool = False


def evaluate(state: OnboardState) -> List[Alarm]:
    """Raw active alarms for `state`, in display order."""
    alarms: List[Alarm] = []
    if state.replay:
        eta = f", ETA {format_elapsed(state.replay_eta_s)}" if state.replay_eta_s else ""
        if state.replay_live_panels:
            count = f"{state.replay_backlog_frames} queued frames" if state.replay_backlog_frames else "queued frames"
            alarms.append(Alarm("REPLAY", f"BACKLOG — onboard replaying {count}{eta}; "
                                          "panels are LIVE, the replay fills plots and logs", AMBER))
        else:
            alarms.append(Alarm("REPLAY", f"REPLAY — onboard backlog {format_elapsed(state.replay_behind_s)} behind{eta}; "
                                          "panels show the last LIVE frame, not the replay", AMBER))
    if state.have_packet:
        if state.flag("OVERTEMP_FAIL"):
            alarms.append(Alarm("OVERTEMP", "OVERTEMP latched — heaters forced off until RESET_CTRL"))
        if state.flag("SAMPLE_TEMP_FAIL"):
            invalid = [f"S{i}" for i in range(6) if not state.heater_temp_valid(i)]
            detail = " ".join(invalid) if invalid else "a heated channel"
            alarms.append(Alarm("SAMPLE_TEMP", f"SAMPLE_TEMP FAIL — {detail} invalid"))
        if state.fallback:
            age = f" ({state.link_loss_s:.0f} s)" if state.link_loss_s is not None else ""
            alarms.append(Alarm("FALLBACK", f"LINK-LOSS FALLBACK active onboard{age}"))
        if state.flag("SEQ_PAUSED"):
            alarms.append(Alarm("SEQ_PAUSED", "bend sequence paused / faulted — BENDSEQ_STATUS"))
        if state.flag("ENERGY_FAIL") or state.budget_exhausted:
            alarms.append(Alarm("ENERGY", "heater energy budget exhausted — heaters latched off"))
        elif (state.energy_wh is not None and state.budget_wh and
              state.energy_wh >= 0.8 * state.budget_wh):
            alarms.append(Alarm("ENERGY", f"heater energy {state.energy_wh:.1f} Wh ≥ 80 % of the "
                                          f"{state.budget_wh:.0f} Wh budget — reduce targets or duty", AMBER))
        if not state.rtc_valid:
            alarms.append(Alarm("RTC", "RTC invalid — onboard timestamps unreliable (logs affected)", AMBER))
        if state.debug_armed:
            alarms.append(Alarm("DEBUG_ARM", "DEBUG ARM active — open-loop heater duty allowed "
                                             "without PT100 feedback (DISARM_DEBUG to end)", AMBER))
        for motor_id in range(2):
            comp = state.component_state.get(f"MOTOR{motor_id}")
            if comp == "FAILED":
                alarms.append(Alarm(f"MOTOR{motor_id}", f"M{motor_id} FAILED — CHECK MOTOR{motor_id}"))
            thermal = state.motor(motor_id).thermal
            if thermal == "hot":
                alarms.append(Alarm(f"M{motor_id}_TEMP",
                                    f"M{motor_id} driver OVER-TEMPERATURE (≥150 °C die) — "
                                    "motor disabled by safety; let it cool, then ENABLE"))
            elif thermal == "warn":
                alarms.append(Alarm(f"M{motor_id}_TEMP",
                                    f"M{motor_id} driver hot (≥120 °C die pre-warning) — "
                                    "reduce run current or duty", AMBER))
        if state.heaters_inhibited:
            moving = [f"M{m.motor_id}" for m in state.motors if m.moving or m.holding]
            who = f" ({' '.join(moving)} moving)" if moving else ""
            alarms.append(Alarm("HEATERS_INHIBITED", f"heaters inhibited{who}", AMBER))
        bad_components = [f"{k}:{v}" for k, v in state.component_state.items()
                          if k in _SENSOR_COMPONENTS and v in _COMPONENT_BAD]
        bad_flags = [f for f in _SENSOR_FLAGS if state.flag(f)]
        if bad_components or bad_flags:
            alarms.append(Alarm("SENSOR", "SENSOR — " + " ".join(bad_components + bad_flags)))
        if state.queue_depth is not None and state.queue_depth > QUEUE_ALARM_FRAMES:
            if not (state.replay and state.replay_live_panels):  # the BACKLOG alarm already says so
                alarms.append(Alarm("RX_QUEUE", f"onboard queue backlog {state.queue_depth} frames (draining)", AMBER))
        if state.plan_state == "running":
            alarms.append(Alarm("PLAN", "fallback plan RUNNING onboard — autonomous bend in progress", AMBER))
        elif state.plan_state == "failed":
            alarms.append(Alarm("PLAN", "fallback plan FAILED onboard — FALLBACK_STATUS for detail"))
    if (not state.silence and state.link_age_s is not None and state.link_age_s >= LINK_STALE_S):
        alarms.append(Alarm("LINK", f"LINK STALE — no frame for {state.link_age_s:.0f} s"))
    return alarms


class AlarmModel:
    """Keeps acknowledgement state across evaluations."""

    def __init__(self) -> None:
        self._acked: Dict[str, str] = {}   # key -> text it was acked with
        self._previous: Dict[str, Alarm] = {}
        self._new_keys: List[str] = []

    def update(self, state: OnboardState) -> List[Alarm]:
        raw = evaluate(state)
        current: Dict[str, Alarm] = {}
        self._new_keys = []
        for alarm in raw:
            acked = alarm.key in self._acked
            current[alarm.key] = Alarm(alarm.key, alarm.text, alarm.severity, acked)
            if alarm.key not in self._previous:
                self._new_keys.append(alarm.key)
        # Acknowledgements die with the condition they acknowledged.
        for key in list(self._acked):
            if key not in current:
                del self._acked[key]
        self._previous = current
        return list(current.values())

    def acknowledge(self, key: str) -> None:
        alarm = self._previous.get(key)
        if alarm is None:
            return
        self._acked[key] = alarm.text
        # Reflect it immediately (not only on the next update) so the
        # strip repaints the chip dimmed on the click that acked it.
        self._previous[key] = Alarm(alarm.key, alarm.text, alarm.severity, True)

    def acknowledge_all(self) -> None:
        for key in list(self._previous):
            self.acknowledge(key)

    @property
    def new_keys(self) -> List[str]:
        """Keys raised by the most recent update that were not active
        before it -- what the (optional) beep fires on."""
        return list(self._new_keys)

    @property
    def active(self) -> List[Alarm]:
        return list(self._previous.values())

    @property
    def unacked_count(self) -> int:
        return sum(1 for a in self._previous.values() if not a.acked)
