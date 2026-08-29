"""Center plots (redesign spec §5.6): time axis in mission elapsed time,
trailing-window selector, full-session retention through `SeriesStore`,
five pages (Temperatures, Ambient, Heaters, Resistance, Motors), pull
markers, target overlays, crosshair readout, PNG/CSV export.
"""
from __future__ import annotations

import csv
import time
from datetime import datetime, timezone
from typing import Dict, List, Optional, Tuple

import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtWidgets import (
    QFileDialog, QHBoxLayout, QLabel, QPushButton, QTabWidget, QVBoxLayout, QWidget,
)

from ..protocol import PullEvent, TelemetryPacket
from ..session_dir import session_epoch
from .series_store import SeriesStore, format_elapsed, window_bounds
from .theme import (
    HEATER_COLORS, HEATER_LABELS, OVERTEMP_CUTOFF_C, PRE_FLOAT_PRESSURE_MBAR,
    RESISTANCE_COLORS, SAMPLE_FLOOR_C,
)
from .widgets import MONO_CSS, MUTED, style_button

MOTOR_COLORS = (("#2ecc71", "#27ae60"), ("#e67e22", "#d35400"))
WINDOWS: List[Tuple[str, Optional[float]]] = [
    ("5 min", 300.0), ("30 min", 1800.0), ("2 h", 7200.0), ("4 h", 14400.0), ("all", None)]
REDRAW_MS = 200


class MissionTimeAxis(pg.AxisItem):
    """Bottom axis labelled T+hh:mm:ss from the mission start (UTC clock
    time until the first frame arrives)."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.t0: Optional[float] = None

    def tickStrings(self, values, scale, spacing):  # noqa: N802
        if self.t0 is None:
            return [datetime.fromtimestamp(v, tz=timezone.utc).strftime("%H:%M:%S") for v in values]
        return [("T+" if v >= self.t0 else "T-") + format_elapsed(abs(v - self.t0)) for v in values]


class TimePlot(QWidget):
    """One pyqtgraph plot bound to a SeriesStore with a legend row that
    shows the latest value of every series."""

    def __init__(self, title: str, y_label: str, unit: str = "", *, show_legend: bool = True,
                 decimals: int = 2, parent=None):
        super().__init__(parent)
        self.unit = unit
        self.decimals = decimals
        self.axis = MissionTimeAxis(orientation="bottom")
        self.plot = pg.PlotWidget(axisItems={"bottom": self.axis})
        self.plot.setTitle(title, color="#dddddd", size="10pt")
        self.plot.setLabel("left", y_label, units=unit)
        # No SI auto-prefixing: it invents units like "kµst"/"kΩ" as the
        # range grows. Values are shown in the unit named, always.
        self.plot.getAxis("left").enableAutoSIPrefix(False)
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.setMenuEnabled(True)
        self.plot.setClipToView(True)
        self.plot.setDownsampling(auto=True, mode="peak")
        self.plot.plotItem.vb.sigRangeChangedManually.connect(self._on_manual_range)
        self._curves: Dict[str, pg.PlotDataItem] = {}
        self._colors: Dict[str, str] = {}
        self._legend: Dict[str, QLabel] = {}
        self._markers: List[pg.InfiniteLine] = []
        self._threshold_lines: Dict[str, pg.InfiniteLine] = {}
        self.follow = True
        self.manual_range_changed = None  # callable set by PlotArea

        lay = QVBoxLayout(self); lay.setContentsMargins(0, 0, 0, 0); lay.setSpacing(2)
        lay.addWidget(self.plot, 1)
        self._legend_row = QWidget()
        self._legend_lay = QHBoxLayout(self._legend_row)
        self._legend_lay.setContentsMargins(8, 0, 8, 2); self._legend_lay.setSpacing(14)
        self._legend_row.setVisible(show_legend)
        lay.addWidget(self._legend_row)
        self._readout = QLabel(""); self._readout.setStyleSheet(f"{MONO_CSS} color: {MUTED}; font-size: 8pt;")
        self._legend_lay.addWidget(self._readout)
        self._legend_lay.addStretch()

        self._vline = pg.InfiniteLine(angle=90, movable=False, pen=pg.mkPen("#666", width=1))
        self._hline = pg.InfiniteLine(angle=0, movable=False, pen=pg.mkPen("#666", width=1))
        self.plot.addItem(self._vline, ignoreBounds=True)
        self.plot.addItem(self._hline, ignoreBounds=True)
        self._proxy = pg.SignalProxy(self.plot.scene().sigMouseMoved, rateLimit=20, slot=self._on_mouse_moved)
        self._store: Optional[SeriesStore] = None

    # -- setup -----------------------------------------------------------------
    def add_series(self, name: str, color: str, width: float = 1.6, dashed: bool = False,
                   legend: bool = True) -> None:
        pen = pg.mkPen(color=color, width=width, style=Qt.PenStyle.DashLine if dashed else Qt.PenStyle.SolidLine)
        self._curves[name] = self.plot.plot([], [], pen=pen, name=name, connect="finite")
        self._colors[name] = color
        if legend:
            lbl = QLabel(f"{name} —")
            lbl.setStyleSheet(f"{MONO_CSS} color: {color}; font-size: 9pt;")
            self._legend[name] = lbl
            self._legend_lay.insertWidget(self._legend_lay.count() - 2, lbl)

    def add_threshold(self, key: str, y_value: float, color: str, label: str) -> None:
        line = pg.InfiniteLine(pos=y_value, angle=0, pen=pg.mkPen(color, width=1, style=Qt.PenStyle.DashLine),
                               label=label, labelOpts={"color": color, "position": 0.97})
        self.plot.addItem(line, ignoreBounds=True)
        self._threshold_lines[key] = line

    def set_threshold(self, key: str, y_value: Optional[float]) -> None:
        line = self._threshold_lines.get(key)
        if line is None:
            return
        line.setVisible(y_value is not None)
        if y_value is not None:
            line.setPos(y_value)

    def add_marker(self, t: float, color: str, label: str) -> None:
        line = pg.InfiniteLine(pos=t, angle=90, pen=pg.mkPen(color, width=1, style=Qt.PenStyle.DotLine),
                               label=label, labelOpts={"color": color, "position": 0.9, "rotateAxis": (1, 0)})
        self.plot.addItem(line, ignoreBounds=True)
        self._markers.append(line)

    def clear_markers(self) -> None:
        for line in self._markers:
            self.plot.removeItem(line)
        self._markers.clear()

    def bind(self, store: SeriesStore) -> None:
        self._store = store

    # -- drawing ---------------------------------------------------------------
    def redraw(self, t_min: Optional[float], t_max: Optional[float], t0: Optional[float]) -> None:
        if self._store is None:
            return
        self.axis.t0 = t0
        for name, curve in self._curves.items():
            t, y = self._store.window(name, t_min, t_max)
            curve.setData(t, y)
            lbl = self._legend.get(name)
            if lbl is not None:
                latest = self._store.latest(name)
                if latest is None or not np.isfinite(latest):
                    lbl.setText(f"{name} —")
                else:
                    lbl.setText(f"{name} {latest:.{self.decimals}f} {self.unit}".rstrip())
        if self.follow and t_max is not None and t_min is not None:
            self.plot.setXRange(t_min, t_max, padding=0.01)
        elif self.follow and t_min is None and self._store.t0 is not None and self._store.t_last is not None:
            span = max(60.0, self._store.t_last - self._store.t0)
            self.plot.setXRange(self._store.t0, self._store.t0 + span, padding=0.01)

    def _on_manual_range(self, *_args) -> None:
        self.follow = False
        if self.manual_range_changed is not None:
            self.manual_range_changed()

    def _on_mouse_moved(self, evt) -> None:
        pos = evt[0]
        vb = self.plot.plotItem.vb
        if not self.plot.sceneBoundingRect().contains(pos) or self._store is None:
            return
        pt = vb.mapSceneToView(pos)
        self._vline.setPos(pt.x()); self._hline.setPos(pt.y())
        parts = []
        try:
            stamp = datetime.fromtimestamp(pt.x(), tz=timezone.utc).strftime("%H:%M:%SZ")
        except (OSError, OverflowError, ValueError):
            stamp = "—"
        elapsed = format_elapsed(abs(pt.x() - self.axis.t0)) if self.axis.t0 is not None else "—"
        parts.append(f"T+{elapsed} · {stamp}")
        for name in self._curves:
            value = self._store.last_before(name, pt.x())
            if value is not None and np.isfinite(value):
                parts.append(f"{name} {value:.{self.decimals}f} {self.unit}".rstrip())
        self._readout.setText("   ".join(parts))

    def series_names(self) -> List[str]:
        return list(self._curves)


class AmbientPage(QWidget):
    """Three x-linked stacked plots: ambient temperature, pressure, UV."""

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(0, 0, 0, 0); lay.setSpacing(2)
        self.temp = TimePlot("Ambient temperature", "T", "°C", decimals=1)
        self.pressure = TimePlot("Ambient pressure", "p", "mbar", decimals=1)
        self.uv = TimePlot("UV (GUVA-S12SD via ADS1115)", "UV", "V", decimals=3)
        self.temp.add_series("AT", "#3498db")
        self.pressure.add_series("AP", "#1abc9c")
        self.pressure.add_threshold("pre_float", PRE_FLOAT_PRESSURE_MBAR, "#f39c12",
                                    f"pre-float {PRE_FLOAT_PRESSURE_MBAR:.0f} mbar")
        self.uv.add_series("UV", "#f1c40f")
        for plot in (self.temp, self.pressure, self.uv):
            lay.addWidget(plot, 1)
        self.pressure.plot.setXLink(self.temp.plot)
        self.uv.plot.setXLink(self.temp.plot)

    @property
    def plots(self) -> List[TimePlot]:
        return [self.temp, self.pressure, self.uv]


class PlotArea(QWidget):
    """The centre region: toolbar + tabbed plots over one SeriesStore."""

    paused_changed = pyqtSignal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.store = SeriesStore()
        self._t0: Optional[float] = None
        self._session = ""
        self._span: Optional[float] = 1800.0
        self._paused = False
        self._pull_markers: List[Tuple[float, int, int]] = []

        lay = QVBoxLayout(self); lay.setContentsMargins(0, 0, 0, 0); lay.setSpacing(0)
        bar = QWidget(); bl = QHBoxLayout(bar); bl.setContentsMargins(8, 4, 8, 0); bl.setSpacing(4)
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(True)
        bl.addStretch()
        win_lbl = QLabel("window"); win_lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        bl.addWidget(win_lbl)
        self._window_buttons: List[QPushButton] = []
        for label, span in WINDOWS:
            btn = QPushButton(label); btn.setCheckable(True)
            style_button(btn, "neutral", min_height=22)
            btn.clicked.connect(lambda _c=False, s=span, b=btn: self.set_window(s))
            bl.addWidget(btn); self._window_buttons.append(btn)
        self.btn_follow = QPushButton("FOLLOW"); self.btn_follow.setCheckable(True); self.btn_follow.setChecked(True)
        style_button(self.btn_follow, "primary", min_height=22)
        self.btn_follow.clicked.connect(self._follow_clicked)
        self.btn_pause = QPushButton("PAUSE"); self.btn_pause.setCheckable(True)
        style_button(self.btn_pause, "neutral", min_height=22)
        self.btn_pause.clicked.connect(lambda: self.set_paused(self.btn_pause.isChecked()))
        self.btn_export = QPushButton("EXPORT"); style_button(self.btn_export, "neutral", min_height=22)
        self.btn_export.clicked.connect(self.export_dialog)
        for btn in (self.btn_follow, self.btn_pause, self.btn_export):
            bl.addWidget(btn)
        lay.addWidget(bar)
        lay.addWidget(self.tabs, 1)

        # Pages.
        self.temps = TimePlot("Specimen temperatures", "T", "°C", decimals=1)
        for i in range(8):
            self.temps.add_series(f"S{i}", RESISTANCE_COLORS[i % len(RESISTANCE_COLORS)])
        for i in range(6):
            self.temps.add_series(f"T{i}", HEATER_COLORS[i % len(HEATER_COLORS)], width=1.0, dashed=True, legend=False)
        self.temps.add_threshold("floor", SAMPLE_FLOOR_C, "#2ecc71", f"fallback floor {SAMPLE_FLOOR_C:.0f} °C")
        self.temps.add_threshold("overtemp", OVERTEMP_CUTOFF_C, "#e74c3c", f"over-T {OVERTEMP_CUTOFF_C:.0f} °C")
        self.ambient = AmbientPage()
        self.heaters = TimePlot("Heater duty", "duty", "%", decimals=0)
        for i, label in enumerate(HEATER_LABELS):
            self.heaters.add_series(label, HEATER_COLORS[i % len(HEATER_COLORS)])
        self.resistance = TimePlot("Specimen resistance (MAX31865)", "R", "Ω")
        self._resistance_series_added: set = set()
        # Millimetres of linear travel (the ball-screw-lead-derived mm/mm_tgt
        # telemetry keys), not microsteps: "kµst" axis labels helped nobody.
        self.motors = TimePlot("Motor position", "position", "mm", decimals=3)
        for motor_id, (c_pos, c_tgt) in enumerate(MOTOR_COLORS):
            self.motors.add_series(f"M{motor_id} pos", c_pos, width=1.8)
            self.motors.add_series(f"M{motor_id} tgt", c_tgt, width=1.0, dashed=True)
        self.tabs.addTab(self.temps, "Temperatures")
        self.tabs.addTab(self.ambient, "Ambient")
        self.tabs.addTab(self.heaters, "Heaters")
        self.tabs.addTab(self.resistance, "Resistance")
        self.tabs.addTab(self.motors, "Motors")
        for plot in self.all_plots():
            plot.bind(self.store)
            plot.manual_range_changed = self._manual_range
        self._dirty = False
        self.tabs.currentChanged.connect(lambda _i: self.redraw(force=True))
        self.set_window(1800.0)

        self._timer = QTimer(self); self._timer.timeout.connect(self.redraw); self._timer.start(REDRAW_MS)

    # -- helpers -----------------------------------------------------------------
    def all_plots(self) -> List[TimePlot]:
        return [self.temps, *self.ambient.plots, self.heaters, self.resistance, self.motors]

    def current_plots(self) -> List[TimePlot]:
        page = self.tabs.currentWidget()
        if isinstance(page, AmbientPage):
            return page.plots
        return [page] if isinstance(page, TimePlot) else []

    def set_window(self, span: Optional[float]) -> None:
        self._span = span
        for btn, (_label, s) in zip(self._window_buttons, WINDOWS):
            btn.setChecked(s == span)
            style_button(btn, "primary" if s == span else "neutral", min_height=22)
        for plot in self.all_plots():
            plot.follow = True
        self.btn_follow.setChecked(True)
        self.redraw(force=True)

    def window_span(self) -> Optional[float]:
        return self._span

    def _follow_clicked(self) -> None:
        follow = self.btn_follow.isChecked()
        for plot in self.all_plots():
            plot.follow = follow
        if follow:
            self.redraw(force=True)

    def _manual_range(self) -> None:
        self.btn_follow.setChecked(False)

    def set_paused(self, paused: bool) -> None:
        self._paused = paused
        self.btn_pause.setChecked(paused)
        self.btn_pause.setText("RESUME" if paused else "PAUSE")
        self.paused_changed.emit(paused)
        if not paused:
            self.redraw(force=True)

    def toggle_paused(self) -> bool:
        self.set_paused(not self._paused)
        return self._paused

    @property
    def paused(self) -> bool:
        return self._paused

    # -- data -------------------------------------------------------------------------
    def on_packet(self, pkt: TelemetryPacket, rx_time: Optional[float] = None) -> None:
        t = rx_time if rx_time is not None else time.time()
        if pkt.session_id != self._session:
            self._session = pkt.session_id
            epoch = session_epoch(pkt.session_id)
            self._t0 = float(epoch) if epoch is not None else t
        values: Dict[str, float] = {}
        for i, temp in enumerate(pkt.sample_temps_c[:8]):
            if pkt.sensor_valid.get(f"S{i}", True) and np.isfinite(temp):
                values[f"S{i}"] = float(temp)
        if pkt.sensor_valid.get("AT", True) and np.isfinite(pkt.ambient_temp_c):
            values["AT"] = float(pkt.ambient_temp_c)
        if pkt.sensor_valid.get("AP", True) and np.isfinite(pkt.ambient_pressure_mbar):
            values["AP"] = float(pkt.ambient_pressure_mbar)
        if pkt.sensor_valid.get("UV", True) and np.isfinite(pkt.uv):
            values["UV"] = float(pkt.uv)
        for i, duty in enumerate(pkt.heater_duty[:6]):
            values[f"H{i}"] = float(duty) * 100.0
        for i, r in enumerate(pkt.sample_resistance_ohm[:8]):
            if r is not None:
                name = f"R{i}"
                values[name] = float(r)
                if name not in self._resistance_series_added:
                    self._resistance_series_added.add(name)
                    self.resistance.add_series(name, RESISTANCE_COLORS[i % len(RESISTANCE_COLORS)])
        for snap in pkt.steppers[:2]:
            m = int(snap.get("motor_id", 0))
            if snap.get("mm") is not None and snap.get("mm_tgt") is not None:
                values[f"M{m} pos"] = float(snap["mm"])
                values[f"M{m} tgt"] = float(snap["mm_tgt"])
            else:
                # Pre-mm firmware / replayed old logs: derive mm from
                # microsteps at the commissioning geometry (200 full-steps
                # per rev, 2 mm ball-screw lead, divisor from the frame).
                us = max(1, int(snap.get("microstep") or 4))
                per_mm = 200.0 * us / 2.0
                values[f"M{m} pos"] = float(snap["position"]) / per_mm
                values[f"M{m} tgt"] = float(snap["target"]) / per_mm
        self.store.append(t, values)
        self._dirty = True

    def set_targets(self, targets: List[Optional[float]], rx_time: Optional[float] = None) -> None:
        """Dashed target overlays on the temperature page (one per heater)."""
        t = rx_time if rx_time is not None else time.time()
        values = {f"T{i}": (float(v) if v is not None else np.nan) for i, v in enumerate(targets[:6])}
        self.store.append(t, values)
        self._dirty = True

    def on_pull_event(self, ev: PullEvent, rx_time: Optional[float] = None) -> None:
        t = rx_time if rx_time is not None else time.time()
        self._pull_markers.append((t, ev.motor_id, ev.pull_id))
        color = MOTOR_COLORS[ev.motor_id % 2][0]
        label = f"pull M{ev.motor_id} #{ev.pull_id}"
        self.resistance.add_marker(t, color, label)
        self.motors.add_marker(t, color, label)
        self.temps.add_marker(t, color, label)

    def clear(self) -> None:
        self.store.clear()
        self._t0 = None
        self._session = ""
        for plot in self.all_plots():
            plot.clear_markers()
        self.redraw(force=True)

    # -- drawing ------------------------------------------------------------------------
    def redraw(self, force: bool = False) -> None:
        if self._paused and not force:
            return
        if not self._dirty and not force:
            return
        self._dirty = False
        t_min, t_max = window_bounds(self.store.t_last, self._span)
        if t_max is None and self.store.t_last is not None:
            t_max = self.store.t_last
        if t_min is not None and self._t0 is not None and t_min < self._t0:
            t_min = self._t0
        for plot in self.current_plots():
            plot.redraw(t_min, t_max if self._span is not None else None, self._t0)

    # -- export ---------------------------------------------------------------------------
    def export_dialog(self) -> None:
        path, selected = QFileDialog.getSaveFileName(
            self, "Export current plot", "coatheal_plot.png", "PNG image (*.png);;CSV of visible window (*.csv)")
        if not path:
            return
        if path.lower().endswith(".csv") or "CSV" in selected:
            self.export_csv(path)
        else:
            self.export_png(path)

    def export_png(self, path: str) -> None:
        plots = self.current_plots()
        if not plots:
            return
        from pyqtgraph.exporters import ImageExporter
        exporter = ImageExporter(plots[0].plot.plotItem)
        exporter.export(path)

    def export_csv(self, path: str) -> None:
        plots = self.current_plots()
        if not plots:
            return
        if plots[0].follow:
            t_min, t_max = window_bounds(self.store.t_last, self._span)
        else:  # export exactly what is on screen
            t_min, t_max = plots[0].plot.plotItem.vb.viewRange()[0]
        units = {name: plot.unit for plot in plots for name in plot.series_names()}
        names: List[str] = []
        for plot in plots:
            names.extend(plot.series_names())
        with open(path, "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["utc", "t_plus_s", "series", "value", "unit"])
            for name in names:
                t, y = self.store.window(name, t_min, t_max)
                for ti, yi in zip(t, y):
                    stamp = datetime.fromtimestamp(float(ti), tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
                    elapsed = "" if self._t0 is None else f"{float(ti) - self._t0:.3f}"
                    writer.writerow([stamp, elapsed, name, f"{float(yi):.6g}", units.get(name, "")])
