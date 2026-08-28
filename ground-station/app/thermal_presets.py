"""Thermal presets v2 (redesign spec §5.3.4).

A preset is the complete thermal set-up the Pi does not persist: six
targets (None = channel off) and PID gains for ALL plus optional
per-channel overrides. Stored in `profiles/thermal_presets.json`, written
atomically (temp file + rename) with the previous file kept as `.bak`.
Legacy v1 `thermal_profiles.json` files are migrated on first load and
never deleted.
"""
from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .reply_format import parse_kv_body

PRESET_VERSION = 2
DEFAULT_PRESET_PATH = Path("profiles/thermal_presets.json")
LEGACY_PROFILE_PATH = Path("profiles/thermal_profiles.json")
HEATER_COUNT = 6
DEFAULT_GAINS = (0.20, 0.02, 0.03)


def _utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


@dataclass
class ThermalPreset:
    name: str
    targets_c: List[Optional[float]] = field(default_factory=lambda: [None] * HEATER_COUNT)
    pid_all: Tuple[float, float, float] = DEFAULT_GAINS
    pid_channels: Dict[int, Tuple[float, float, float]] = field(default_factory=dict)
    note: str = ""
    created_utc: str = ""
    last_applied_utc: Optional[str] = None

    def to_json(self) -> Dict[str, Any]:
        return {
            "targets_c": [None if t is None else float(t) for t in self.targets_c],
            "pid_all": {"kp": self.pid_all[0], "ki": self.pid_all[1], "kd": self.pid_all[2]},
            "pid_channels": {str(i): {"kp": g[0], "ki": g[1], "kd": g[2]}
                             for i, g in sorted(self.pid_channels.items())},
            "note": self.note,
            "created_utc": self.created_utc,
            "last_applied_utc": self.last_applied_utc,
        }

    @classmethod
    def from_json(cls, name: str, data: Dict[str, Any]) -> "ThermalPreset":
        targets_raw = list(data.get("targets_c", []))[:HEATER_COUNT]
        targets: List[Optional[float]] = []
        for value in targets_raw:
            try:
                targets.append(None if value is None else float(value))
            except (TypeError, ValueError):
                targets.append(None)
        targets += [None] * (HEATER_COUNT - len(targets))
        pid_all = _gains(data.get("pid_all") or data.get("pid"))
        channels: Dict[int, Tuple[float, float, float]] = {}
        for key, gains in (data.get("pid_channels") or {}).items():
            try:
                channels[int(key)] = _gains(gains)
            except (TypeError, ValueError):
                continue
        return cls(name=name, targets_c=targets, pid_all=pid_all, pid_channels=channels,
                   note=str(data.get("note", "")), created_utc=str(data.get("created_utc", "")),
                   last_applied_utc=data.get("last_applied_utc"))

    def commands(self) -> List[str]:
        """Wire commands that reproduce this preset onboard, in a safe
        order: gains first, then targets (clears for channels set to None)."""
        kp, ki, kd = self.pid_all
        cmds = [f"SET_PID ALL {kp:.6g} {ki:.6g} {kd:.6g}"]
        for index, gains in sorted(self.pid_channels.items()):
            cmds.append(f"SET_PID {index} {gains[0]:.6g} {gains[1]:.6g} {gains[2]:.6g}")
        for index, target in enumerate(self.targets_c):
            if target is None:
                cmds.append(f"CLEAR_TEMP_TARGET {index}")
            else:
                cmds.append(f"SET_TEMP_TARGET {index} {target:.3f}")
        return cmds

    @classmethod
    def from_get_thermal(cls, name: str, body: str,
                         gains: Tuple[float, float, float] = DEFAULT_GAINS) -> "ThermalPreset":
        """Capture the onboard's current targets from a GET_THERMAL reply
        (`h0_target=25;h0_temp=...;h0_duty=...;...`, '-' = no target)."""
        kv = parse_kv_body(body)
        targets: List[Optional[float]] = []
        for index in range(HEATER_COUNT):
            raw = kv.get(f"h{index}_target", "-")
            try:
                targets.append(None if raw in ("-", "") else float(raw))
            except ValueError:
                targets.append(None)
        return cls(name=name, targets_c=targets, pid_all=gains, created_utc=_utc())


def _gains(data: Any) -> Tuple[float, float, float]:
    if isinstance(data, dict):
        try:
            return (float(data.get("kp", DEFAULT_GAINS[0])), float(data.get("ki", DEFAULT_GAINS[1])),
                    float(data.get("kd", DEFAULT_GAINS[2])))
        except (TypeError, ValueError):
            return DEFAULT_GAINS
    if isinstance(data, (list, tuple)) and len(data) == 3:
        try:
            return (float(data[0]), float(data[1]), float(data[2]))
        except (TypeError, ValueError):
            return DEFAULT_GAINS
    return DEFAULT_GAINS


class PresetStore:
    def __init__(self, path: Path = DEFAULT_PRESET_PATH,
                 legacy_path: Optional[Path] = None):
        self.path = Path(path)
        self.legacy_path = legacy_path if legacy_path is not None else self.path.with_name(LEGACY_PROFILE_PATH.name)
        self._presets: Dict[str, ThermalPreset] = {}
        self.load_error: Optional[str] = None
        self.migrated_from: Optional[Path] = None

    # -- persistence ---------------------------------------------------------
    def load(self) -> "PresetStore":
        self._presets = {}
        self.load_error = None
        if self.path.exists():
            try:
                payload = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                self.load_error = f"{self.path}: {exc}"
                payload = {}
            for name, data in (payload.get("presets") or {}).items():
                if isinstance(data, dict):
                    self._presets[str(name)] = ThermalPreset.from_json(str(name), data)
        elif self.legacy_path.exists():
            self._migrate_legacy()
        return self

    def _migrate_legacy(self) -> None:
        try:
            payload = json.loads(self.legacy_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            self.load_error = f"{self.legacy_path}: {exc}"
            return
        profiles = payload.get("profiles", payload) if isinstance(payload, dict) else {}
        for name, data in (profiles or {}).items():
            if isinstance(data, dict):
                preset = ThermalPreset.from_json(str(name), data)
                preset.note = preset.note or "migrated from thermal_profiles.json (v1)"
                preset.created_utc = preset.created_utc or _utc()
                self._presets[str(name)] = preset
        self.migrated_from = self.legacy_path
        if self._presets:
            self.save()

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {"version": PRESET_VERSION, "saved_utc": _utc(),
                   "presets": {name: p.to_json() for name, p in sorted(self._presets.items())}}
        tmp = self.path.with_name(self.path.name + ".tmp")
        tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if self.path.exists():
            shutil.copyfile(self.path, self.path.with_name(self.path.name + ".bak"))
        os.replace(tmp, self.path)

    # -- access ---------------------------------------------------------------
    def names(self) -> List[str]:
        return sorted(self._presets)

    def get(self, name: str) -> Optional[ThermalPreset]:
        return self._presets.get(name)

    def put(self, preset: ThermalPreset) -> None:
        if not preset.created_utc:
            preset.created_utc = _utc()
        self._presets[preset.name] = preset
        self.save()

    def delete(self, name: str) -> bool:
        if name not in self._presets:
            return False
        del self._presets[name]
        self.save()
        return True

    def rename(self, old: str, new: str) -> bool:
        if old not in self._presets or not new or new in self._presets:
            return False
        preset = self._presets.pop(old)
        preset.name = new
        self._presets[new] = preset
        self.save()
        return True

    def mark_applied(self, name: str, ts_utc: Optional[str] = None) -> None:
        preset = self._presets.get(name)
        if preset is None:
            return
        preset.last_applied_utc = ts_utc or _utc()
        self.save()
