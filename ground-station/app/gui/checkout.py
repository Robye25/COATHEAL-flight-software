"""Live go/no-go checklist (redesign spec §5.7, replaces Preflight).

Every row is derived from telemetry the operator can see elsewhere; this
is the single-glance summary before ARM and before the bend. Pure.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

from .state import OnboardState

GREEN, AMBER, RED, GRAY = "green", "amber", "red", "gray"
_COMPONENT_BAD = {"DEGRADED", "STALE", "FAILED"}


@dataclass(frozen=True)
class CheckItem:
    key: str
    label: str
    color: str
    note: str = ""


def _component(state: OnboardState, key: str) -> Optional[str]:
    return state.component_state.get(key)


def checkout_items(state: OnboardState, *, link_ok: bool, unacked_alarms: int = 0,
                   last_check: Optional[Dict[str, str]] = None) -> List[CheckItem]:
    items: List[CheckItem] = []
    if not state.have_packet:
        items.append(CheckItem("link", "Telemetry link", RED if not link_ok else AMBER,
                               "no frame received yet"))
        return items

    age = state.link_age_s
    if state.silence:
        items.append(CheckItem("link", "Telemetry link", GRAY, "radio silence"))
    elif not link_ok:
        items.append(CheckItem("link", "Telemetry link", RED, "receiver down"))
    elif age is not None and age >= 5.0:
        items.append(CheckItem("link", "Telemetry link", RED, f"stale {age:.0f} s"))
    elif age is not None and age >= 2.0:
        items.append(CheckItem("link", "Telemetry link", AMBER, f"{age:.1f} s"))
    else:
        items.append(CheckItem("link", "Telemetry link", GREEN, "" if age is None else f"{age:.1f} s"))

    items.append(CheckItem("rtc", "RTC valid", GREEN if state.rtc_valid else RED))

    for key, label in (("DPS310", "DPS310 pressure/ambient"), ("ADS1115", "ADS1115 UV")):
        comp = _component(state, key)
        color = GREEN if comp == "OK" else (GRAY if comp in (None, "DISCOVERING") else
                                            AMBER if comp == "DISABLED" else RED)
        items.append(CheckItem(key.lower(), label, color, comp or "not reported"))

    valid = sum(1 for v in state.sample_temps if v is not None)
    total = len(state.sample_temps)
    items.append(CheckItem("pt100", "PT100 channels valid",
                           GREEN if valid == total else (AMBER if valid else RED),
                           f"{valid}/{total}"))

    clicks = sum(1 for v in state.sample_resistance if v is not None)
    if state.flag("RESISTANCE_FAIL"):
        items.append(CheckItem("max31865", "MAX31865 clicks", RED, "RESISTANCE_FAIL"))
    else:
        items.append(CheckItem("max31865", "MAX31865 clicks",
                               GREEN if clicks >= 2 else (AMBER if clicks == 1 else RED),
                               f"{clicks} specimen{'s' if clicks != 1 else ''} reporting"))

    pwm = _component(state, "PWM")
    items.append(CheckItem("pwm", "PWM heater driver", GREEN if pwm == "OK" else RED, pwm or "not reported"))

    for motor in state.motors:
        key = f"m{motor.motor_id}"
        label = f"M{motor.motor_id} healthy · enabled · zeroed"
        if not motor.present:
            items.append(CheckItem(key, label, GRAY, "no telemetry"))
        elif not motor.healthy or _component(state, f"MOTOR{motor.motor_id}") == "FAILED":
            items.append(CheckItem(key, label, RED, "FAILED"))
        elif not motor.enabled:
            items.append(CheckItem(key, label, AMBER, "not enabled"))
        elif motor.zeroed is False:
            items.append(CheckItem(key, label, AMBER, "not zeroed"))
        elif motor.zeroed is None:
            items.append(CheckItem(key, label, AMBER, "zeroed: unknown (old firmware)"))
        else:
            items.append(CheckItem(key, label, GREEN))

    storage_ok = state.flag("SD_OK") and state.flag("USB_OK")
    storage_note = " ".join(f for f in ("SD_FAIL", "USB_FAIL") if state.flag(f))
    items.append(CheckItem("storage", "Storage SD · USB", GREEN if storage_ok else RED, storage_note))

    if state.budget_exhausted or state.flag("ENERGY_FAIL"):
        items.append(CheckItem("energy", "Energy budget", RED, "exhausted"))
    elif state.energy_wh is not None and state.budget_wh:
        items.append(CheckItem("energy", "Energy budget", GREEN,
                               f"{state.energy_wh:.1f} / {state.budget_wh:.0f} Wh"))
    else:
        items.append(CheckItem("energy", "Energy budget", GREEN if state.flag("ENERGY_OK") else GRAY))

    mode_color = GREEN if state.mode == "RUN" else (RED if state.mode == "SAFE" else AMBER)
    items.append(CheckItem("mode", "Mode RUN (armed)", mode_color, state.mode or "unknown"))

    items.append(CheckItem("alarms", "No unacknowledged alarms",
                           GREEN if unacked_alarms == 0 else RED,
                           "" if unacked_alarms == 0 else f"{unacked_alarms} active"))

    if last_check:
        overall = last_check.get("overall", "")
        failing = [k for k, v in last_check.items() if v == "FAIL" and k != "overall"]
        warnings = [k for k, v in last_check.items() if k.endswith("_warn") and v]
        color = RED if overall != "OK" else (AMBER if warnings else GREEN)
        note = overall + (" — " + " ".join(failing) if failing else "") + (" — " + " ".join(warnings) if warnings else "")
        items.append(CheckItem("check", "Last CHECK", color, note))
    return items
