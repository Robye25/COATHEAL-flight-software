from __future__ import annotations

import json
from pathlib import Path
from typing import Any


DEFAULT_PROFILE_PATH = Path("profiles/thermal_profiles.json")


def load_profiles(path: Path = DEFAULT_PROFILE_PATH) -> dict[str, dict[str, Any]]:
    if not path.exists():
        return {}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError):
        return {}
    if not isinstance(payload, dict):
        return {}
    profiles = payload.get("profiles", payload)
    if not isinstance(profiles, dict):
        return {}
    return {
        str(name): value
        for name, value in profiles.items()
        if isinstance(value, dict)
    }


def save_profiles(
    profiles: dict[str, dict[str, Any]],
    path: Path = DEFAULT_PROFILE_PATH,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"version": 1, "profiles": profiles}
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def build_profile(targets: list[float], kp: float, ki: float,
                  kd: float) -> dict[str, Any]:
    return {
        "targets_c": [float(value) for value in targets],
        "pid": {"kp": float(kp), "ki": float(ki), "kd": float(kd)},
    }
