"""Right column — latest value of every telemetry field (compact
monospace table)."""
from __future__ import annotations

import math
from typing import Dict

from PyQt6.QtWidgets import QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget

from ..protocol import TelemetryPacket
from .state import OnboardState
from .widgets import MONO_CSS, MUTED, SectionLabel


class ValuesPanel(QScrollArea):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        inner = QWidget(); self.setWidget(inner)
        self._lay = QVBoxLayout(inner); self._lay.setContentsMargins(6, 6, 6, 6); self._lay.setSpacing(2)
        self._fields: Dict[str, QLabel] = {}
        self._section("Session")
        for key, label in (("session_id", "session"), ("seq", "seq"), ("timestamp", "onboard UTC"),
                           ("rtc_valid", "rtc_valid"), ("phase", "phase"), ("mode", "mode")):
            self._row(key, label)
        self._section("Environment")
        for key, label in (("ambient_temp_c", "ambient T °C"), ("ambient_pressure_mbar", "pressure mbar"), ("uv", "UV V")):
            self._row(key, label)
        self._section("Samples °C")
        for i in range(8):
            self._row(f"sample_{i}", f"S{i}")
        self._section("Resistance Ω")
        for i in range(8):
            self._row(f"resistance_{i}", f"R{i}")
        self._section("Heaters duty")
        for i in range(6):
            self._row(f"heater_{i}", f"H{i}")
        self._section("Motors")
        for m in range(2):
            self._row(f"m{m}_state", f"M{m} pos / tgt")
            self._row(f"m{m}_cfg", f"M{m} Hz · µstep")
            self._row(f"m{m}_flags", f"M{m} flags")
            self._row(f"m{m}_src", f"M{m} src · seq")
        self._section("Control")
        for key, label in (("fallback", "fallback"), ("link_loss_s", "link loss s"), ("energy", "energy Wh"),
                           ("heaters_active", "heaters active"), ("queue", "onboard queue"), ("plan", "fallback plan")):
            self._row(key, label)
        self._lay.addStretch()

    def _section(self, title: str) -> None:
        self._lay.addWidget(SectionLabel(title))

    def _row(self, key: str, label: str) -> None:
        w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0); h.setSpacing(6)
        name = QLabel(label); name.setMinimumWidth(96); name.setStyleSheet(f"color: {MUTED};")
        val = QLabel("—"); val.setStyleSheet(MONO_CSS)
        h.addWidget(name); h.addWidget(val, 1)
        self._lay.addWidget(w)
        self._fields[key] = val

    def on_packet(self, pkt: TelemetryPacket, state: OnboardState) -> None:
        def reading(key: str, value: float, precision: int) -> str:
            valid = pkt.sensor_valid.get(key, True)
            if valid and math.isfinite(value):
                return f"{value:.{precision}f}"
            age = pkt.sensor_age_ms.get(key, -1)
            if math.isfinite(value) and age >= 0:
                return f"{value:.{precision}f} stale {age / 1000.0:.1f}s"
            return "N/A"
        f = self._fields
        f["session_id"].setText(pkt.session_id)
        f["seq"].setText(str(pkt.seq))
        f["timestamp"].setText(pkt.timestamp)
        f["rtc_valid"].setText("1" if pkt.rtc_valid else "0")
        f["phase"].setText(pkt.phase)
        f["mode"].setText(pkt.mode or "—")
        f["ambient_temp_c"].setText(reading("AT", pkt.ambient_temp_c, 2))
        f["ambient_pressure_mbar"].setText(reading("AP", pkt.ambient_pressure_mbar, 1))
        f["uv"].setText(reading("UV", pkt.uv, 3))
        for i in range(8):
            f[f"sample_{i}"].setText(reading(f"S{i}", pkt.sample_temps_c[i], 2) if i < len(pkt.sample_temps_c) else "N/A")
            r = pkt.sample_resistance_ohm[i] if i < len(pkt.sample_resistance_ohm) else None
            f[f"resistance_{i}"].setText("—" if r is None else f"{r:.2f}")
        for i in range(6):
            f[f"heater_{i}"].setText(f"{pkt.heater_duty[i] * 100:.0f} %" if i < len(pkt.heater_duty) else "—")
        for m in range(2):
            motor = state.motor(m)
            if not motor.present:
                for k in ("state", "cfg", "flags", "src"):
                    f[f"m{m}_{k}"].setText("—")
                continue
            f[f"m{m}_state"].setText(f"{motor.position} / {motor.target}")
            f[f"m{m}_cfg"].setText(f"{motor.hz:.0f} Hz · µ{motor.microstep}")
            flags = []
            flags.append("EN" if motor.enabled else "dis")
            flags.append("zeroed" if motor.zeroed else ("zero?" if motor.zeroed is None else "unzeroed"))
            flags.append("MOVING" if motor.moving else ("HOLD" if motor.holding else "idle"))
            flags.append("ok" if motor.healthy else "FAILED")
            f[f"m{m}_flags"].setText(" · ".join(flags))
            f[f"m{m}_src"].setText(f"{motor.source or '—'} · {motor.seq_name or '-'}/{motor.seq_state or '-'}")
        f["fallback"].setText("—" if state.fallback is None else ("ACTIVE" if state.fallback else "inactive"))
        f["link_loss_s"].setText("—" if state.link_loss_s is None else f"{state.link_loss_s:.1f}")
        f["energy"].setText("—" if state.energy_wh is None else
                            f"{state.energy_wh:.2f} / {state.budget_wh:.0f}" + (" EXHAUSTED" if state.budget_exhausted else ""))
        f["heaters_active"].setText("—" if state.heaters_active is None else str(state.heaters_active))
        f["queue"].setText("—" if state.queue_depth is None else str(state.queue_depth))
        f["plan"].setText(state.plan_state or "—")
