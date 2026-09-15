"""Right column — latest value of every telemetry field (compact
monospace table), the samples and heaters grouped by motor."""
from __future__ import annotations

import math
from typing import Dict, List, Optional

from PyQt6.QtWidgets import QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget

from ..protocol import TelemetryPacket, mm_s_from_hz
from .state import DEFAULT_LAYOUT, Layout, OnboardState
from .widgets import MONO_CSS, MUTED, SectionLabel


class ValuesPanel(QScrollArea):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        inner = QWidget(); self.setWidget(inner)
        self._lay = QVBoxLayout(inner); self._lay.setContentsMargins(6, 6, 6, 6); self._lay.setSpacing(2)
        self._fields: Dict[str, QLabel] = {}
        self._layout: Layout = DEFAULT_LAYOUT
        self._last: Optional[tuple] = None
        self._section("Session")
        for key, label in (("session_id", "session"), ("seq", "seq"), ("timestamp", "onboard UTC"),
                           ("rtc_valid", "rtc_valid"), ("phase", "phase"), ("mode", "mode")):
            self._row(key, label)
        self._section("Environment")
        for key, label in (("ambient_temp_c", "ambient T °C"), ("ambient_pressure_mbar", "pressure mbar"), ("uv", "UV V")):
            self._row(key, label)
        # Per motor group: its samples (with the heater each has), heater
        # duties and the MAX31865 click on it -- rebuilt by set_layout.
        self._groups = QWidget()
        self._groups_lay = QVBoxLayout(self._groups)
        self._groups_lay.setContentsMargins(0, 0, 0, 0); self._groups_lay.setSpacing(2)
        self._lay.addWidget(self._groups)
        self._group_keys: List[str] = []
        self._section("Motors")
        for m in range(2):
            self._row(f"m{m}_state", f"M{m} pos / tgt")
            self._row(f"m{m}_cfg", f"M{m} mm/s · µstep")
            self._row(f"m{m}_flags", f"M{m} flags")
            self._row(f"m{m}_src", f"M{m} src · seq")
        self._section("Control")
        for key, label in (("fallback", "fallback"), ("link_loss_s", "link loss s"), ("energy", "energy Wh"),
                           ("heaters_active", "heaters active"), ("queue", "onboard queue"), ("plan", "fallback plan")):
            self._row(key, label)
        self._lay.addStretch()
        self.set_layout(self._layout)

    # `lay is None`, not `lay or ...`: an empty Qt layout is falsy (len() is
    # its item count), which put the first group rows in the outer layout.
    def _section(self, title: str, lay: Optional[QVBoxLayout] = None) -> None:
        (self._lay if lay is None else lay).addWidget(SectionLabel(title))

    def _row(self, key: str, label: str, lay: Optional[QVBoxLayout] = None) -> None:
        w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0); h.setSpacing(6)
        name = QLabel(label); name.setMinimumWidth(96); name.setStyleSheet(f"color: {MUTED};")
        val = QLabel("—"); val.setStyleSheet(MONO_CSS)
        h.addWidget(name); h.addWidget(val, 1)
        (self._lay if lay is None else lay).addWidget(w)
        self._fields[key] = val

    def set_layout(self, layout: Layout) -> None:
        self._layout = layout
        for key in self._group_keys:
            self._fields.pop(key, None)
        self._group_keys = []
        while self._groups_lay.count():
            old = self._groups_lay.takeAt(0).widget()
            if old is not None:
                old.setParent(None)   # out of the tree now, not at the next event loop pass
                old.deleteLater()
        before = set(self._fields)
        for motor, samples in enumerate(layout.motor_samples):
            self._section(f"Motor {motor} group", self._groups_lay)
            for sample in samples:
                heater = layout.heater_of_sample(sample)
                owner = f"H{heater}" if heater is not None else "unheated"
                self._row(f"sample_{sample}", f"S{sample} °C · {owner}", self._groups_lay)
            for heater in layout.heaters_of_motor(motor):
                self._row(f"heater_{heater}", f"H{heater} duty", self._groups_lay)
            for sample in samples:
                click = layout.click_of_sample(sample)
                if click is not None:
                    # Only the two click-monitored specimens (owner decision
                    # 2026-08-29); every other RESISTANCE slot is always '-'.
                    self._row(f"resistance_{sample}", f"S{sample} Ω · click {click}", self._groups_lay)
        self._group_keys = [key for key in self._fields if key not in before]
        if self._last is not None:
            self.on_packet(*self._last)

    def on_packet(self, pkt: TelemetryPacket, state: OnboardState) -> None:
        self._last = (pkt, state)

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
        f["ambient_temp_c"].setText(reading("AT", pkt.ambient_temp_c, 1))
        f["ambient_pressure_mbar"].setText(reading("AP", pkt.ambient_pressure_mbar, 1))
        f["uv"].setText(reading("UV", pkt.uv, 3))
        for i in range(8):
            if f"sample_{i}" in f:
                f[f"sample_{i}"].setText(reading(f"S{i}", pkt.sample_temps_c[i], 1)
                                         if i < len(pkt.sample_temps_c) else "N/A")
        for i in self._layout.clicks:
            if f"resistance_{i}" in f:
                r = pkt.sample_resistance_ohm[i] if i < len(pkt.sample_resistance_ohm) else None
                f[f"resistance_{i}"].setText("—" if r is None else f"{r:.2f}")
        for i in range(6):
            if f"heater_{i}" in f:
                f[f"heater_{i}"].setText(f"{pkt.heater_duty[i] * 100:.0f} %" if i < len(pkt.heater_duty) else "—")
        for m in range(2):
            motor = state.motor(m)
            if not motor.present:
                for k in ("state", "cfg", "flags", "src"):
                    f[f"m{m}_{k}"].setText("—")
                continue
            if motor.mm is not None and motor.mm_tgt is not None:
                f[f"m{m}_state"].setText(f"{motor.mm:.3f} / {motor.mm_tgt:.3f} mm")
                f[f"m{m}_state"].setToolTip(f"{motor.position} / {motor.target} µsteps")
            else:
                f[f"m{m}_state"].setText(f"{motor.position} / {motor.target} µst")
            cfg = f"{mm_s_from_hz(motor.hz):.2f} mm/s · µstep 1/{motor.microstep}"
            if motor.amps is not None:
                cfg += f" · {motor.amps:.2f} A"
            f[f"m{m}_cfg"].setText(cfg)
            flags = []
            flags.append("EN" if motor.enabled else "dis")
            flags.append("zeroed" if motor.zeroed else ("zero?" if motor.zeroed is None else "unzeroed"))
            flags.append("MOVING" if motor.moving else ("HOLD" if motor.holding else "idle"))
            flags.append("ok" if motor.healthy else "FAILED")
            if motor.thermal == "hot":
                flags.append("DRV OVERTEMP")
            elif motor.thermal == "warn":
                flags.append("drv ≥120 °C")
            f[f"m{m}_flags"].setText(" · ".join(flags))
            f[f"m{m}_src"].setText(f"{motor.source or '—'} · {motor.seq_name or '-'}/{motor.seq_state or '-'}")
        f["fallback"].setText("—" if state.fallback is None else ("ACTIVE" if state.fallback else "inactive"))
        f["link_loss_s"].setText("—" if state.link_loss_s is None else f"{state.link_loss_s:.1f}")
        if state.energy_wh is None:
            energy_text = "—"
        elif state.budget_wh:
            energy_text = f"{state.energy_wh:.2f} / {state.budget_wh:.0f}"
        else:
            energy_text = f"{state.energy_wh:.2f}"  # budget unreported or unlimited
        if state.budget_exhausted:
            energy_text += " EXHAUSTED"
        f["energy"].setText(energy_text)
        f["heaters_active"].setText("—" if state.heaters_active is None else str(state.heaters_active))
        f["queue"].setText("—" if state.queue_depth is None else str(state.queue_depth))
        f["plan"].setText(state.plan_state or "—")
