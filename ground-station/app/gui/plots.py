"""Plot tabs with pause / crosshair / threshold-line upgrades.

The `LivePlotWidget` base class keeps a ring buffer of (seq, value-map) and
provides:
  * `set_paused(bool)` — stops rendering while buffer keeps growing.
  * A shared crosshair with a floating readout label.
  * Helper to add horizontal threshold lines from config.

`PlotTabs` bundles five plots (Temperature, Pressure, Heaters, Env, Stepper)
and fans telemetry packets + pause toggle to each.
"""
from __future__ import annotations

from collections import deque
from typing import Dict

import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QTabWidget, QVBoxLayout, QWidget

from ..protocol import TelemetryPacket
from .theme import (
    ACTIVATION_PRESSURE_MBAR, ACTIVATION_TARGET_C, HEATER_COLORS, HEATER_LABELS,
    MAX_POINTS, OVERTEMP_CUTOFF_C, UNIFORMITY_BAND_C,
)


class LivePlotWidget(QWidget):
    def __init__(self, title: str, y_label: str, unit: str = "", parent=None):
        super().__init__(parent)
        self._plot = pg.PlotWidget()
        self._plot.setTitle(title, color="#dddddd", size="11pt")
        self._plot.setLabel("left", y_label, units=unit)
        self._plot.setLabel("bottom", "Sequence")
        self._plot.addLegend(offset=(10, 10))
        self._plot.showGrid(x=True, y=True, alpha=0.25)
        self._plot.setMenuEnabled(True)
        self._plot.setClipToView(True)

        self._curves: Dict[str, pg.PlotDataItem] = {}
        self._x: deque = deque(maxlen=MAX_POINTS)
        self._y: Dict[str, deque] = {}
        self._paused = False

        lay = QVBoxLayout(self); lay.setContentsMargins(0, 0, 0, 0); lay.addWidget(self._plot)

        # Crosshair
        self._vline = pg.InfiniteLine(angle=90, movable=False, pen=pg.mkPen("#888", width=1))
        self._hline = pg.InfiniteLine(angle=0, movable=False, pen=pg.mkPen("#888", width=1))
        self._plot.addItem(self._vline, ignoreBounds=True)
        self._plot.addItem(self._hline, ignoreBounds=True)
        self._readout = pg.TextItem(color="#eeeeee", anchor=(0, 1), fill=pg.mkBrush(0, 0, 0, 150))
        self._plot.addItem(self._readout)
        self._readout.setPos(0, 0)
        self._proxy = pg.SignalProxy(self._plot.scene().sigMouseMoved,
                                     rateLimit=30, slot=self._on_mouse_moved)

    # ── public API ──
    def add_curve(self, name: str, color: str, width: int = 2) -> None:
        if name in self._curves:
            return
        pen = pg.mkPen(color=color, width=width)
        self._curves[name] = self._plot.plot([], [], pen=pen, name=name)
        self._y[name] = deque(maxlen=MAX_POINTS)

    def add_threshold(self, y_value: float, color: str, label: str) -> None:
        line = pg.InfiniteLine(pos=y_value, angle=0,
                               pen=pg.mkPen(color, width=1, style=Qt.PenStyle.DashLine),
                               label=label, labelOpts={"color": color, "position": 0.95})
        self._plot.addItem(line, ignoreBounds=True)

    def set_paused(self, paused: bool) -> None:
        self._paused = paused

    def push(self, seq: int, values: Dict[str, float]) -> None:
        self._x.append(seq)
        for name, val in values.items():
            if name not in self._y:
                self._y[name] = deque(maxlen=MAX_POINTS)
            self._y[name].append(val)
        if self._paused:
            return
        x = np.fromiter(self._x, dtype=float)
        for name, curve in self._curves.items():
            buf = self._y.get(name)
            if not buf:
                continue
            y = np.fromiter(buf, dtype=float)
            n = min(len(x), len(y))
            curve.setData(x[-n:], y[-n:])

    def clear(self) -> None:
        self._x.clear()
        for d in self._y.values():
            d.clear()
        for c in self._curves.values():
            c.setData([], [])

    # ── crosshair handler ──
    def _on_mouse_moved(self, evt) -> None:
        pos = evt[0]
        vb = self._plot.plotItem.vb
        if not self._plot.sceneBoundingRect().contains(pos):
            return
        pt = vb.mapSceneToView(pos)
        self._vline.setPos(pt.x()); self._hline.setPos(pt.y())
        if not self._x:
            return
        xs = list(self._x)
        ix = min(range(len(xs)), key=lambda i: abs(xs[i] - pt.x()))
        lines = [f"seq {xs[ix]}"]
        for name, buf in self._y.items():
            if name in self._curves and ix < len(buf):
                lines.append(f"{name}: {list(buf)[ix]:.2f}")
        self._readout.setText("\n".join(lines))
        self._readout.setPos(pt.x(), pt.y())


class PlotTabs(QTabWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._temp = LivePlotWidget("Specimen & Box Temperature", "temperature", "°C")
        self._temp.add_curve("Box", "#e67e22", width=3)
        for i in range(9):
            self._temp.add_curve(f"S{i}", HEATER_COLORS[i % len(HEATER_COLORS)])
        self._temp.add_threshold(ACTIVATION_TARGET_C, "#2ecc71", f"target {ACTIVATION_TARGET_C:.0f}°C")
        self._temp.add_threshold(OVERTEMP_CUTOFF_C, "#e74c3c", f"over-T {OVERTEMP_CUTOFF_C:.0f}°C")
        self._temp.add_threshold(ACTIVATION_TARGET_C + UNIFORMITY_BAND_C, "#3498db",
                                 f"unif +{UNIFORMITY_BAND_C:.0f}°C")
        self._temp.add_threshold(ACTIVATION_TARGET_C - UNIFORMITY_BAND_C, "#3498db",
                                 f"unif -{UNIFORMITY_BAND_C:.0f}°C")

        self._pressure = LivePlotWidget("Ambient Pressure", "pressure", "mbar")
        self._pressure.add_curve("ambient", "#3498db", width=2)
        self._pressure.add_threshold(ACTIVATION_PRESSURE_MBAR, "#f39c12",
                                     f"activation {ACTIVATION_PRESSURE_MBAR:.0f} mbar")

        self._heaters = LivePlotWidget("Heater Duty", "duty", "%")
        for i in range(10):
            self._heaters.add_curve(HEATER_LABELS[i], HEATER_COLORS[i % len(HEATER_COLORS)])

        self._env = LivePlotWidget("Environment", "value")
        self._env.add_curve("humidity%", "#3498db")
        self._env.add_curve("UV×100", "#f1c40f")

        self._stepper = LivePlotWidget("Stepper position", "steps")
        self._stepper.add_curve("position", "#2ecc71", width=2)
        self._stepper.add_curve("target",  "#e67e22", width=2)

        self.addTab(self._temp,     "🌡 Temperature")
        self.addTab(self._pressure, "📈 Pressure")
        self.addTab(self._heaters,  "🔥 Heaters")
        self.addTab(self._env,      "🌫 Env")
        self.addTab(self._stepper,  "⚙ Stepper")

    # ── API ──
    def on_packet(self, pkt: TelemetryPacket) -> None:
        seq = pkt.seq
        temps = {"Box": pkt.box_temp_c}
        for i, t in enumerate(pkt.sample_temps_c[:9]):
            temps[f"S{i}"] = t
        self._temp.push(seq, temps)
        self._pressure.push(seq, {"ambient": pkt.ambient_pressure_mbar})
        heaters = {}
        for i, d in enumerate(pkt.heater_duty[:10]):
            heaters[HEATER_LABELS[i]] = d * 100.0
        self._heaters.push(seq, heaters)
        self._env.push(seq, {"humidity%": pkt.ambient_humidity_pct, "UV×100": pkt.uv * 100.0})
        if pkt.stepper is not None:
            self._stepper.push(seq, {"position": float(pkt.stepper.position),
                                     "target":   float(pkt.stepper.target)})

    def toggle_paused(self) -> bool:
        paused = not self._temp._paused  # all share state via set_paused
        for w in (self._temp, self._pressure, self._heaters, self._env, self._stepper):
            w.set_paused(paused)
        return paused

    def clear(self) -> None:
        for w in (self._temp, self._pressure, self._heaters, self._env, self._stepper):
            w.clear()
