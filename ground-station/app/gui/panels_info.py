"""Right-dock + bottom-dock panels: top status strip, values, preflight,
command history, log."""
from __future__ import annotations

import math
import time
from collections import deque
from typing import Deque

from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import (
    QAbstractItemView, QFileDialog, QGridLayout, QGroupBox, QHBoxLayout, QLabel,
    QListWidget, QListWidgetItem, QPushButton, QScrollArea, QTableWidget,
    QTableWidgetItem, QTextEdit, QVBoxLayout, QWidget,
)

from ..protocol import PullEvent, TelemetryPacket
from .dispatch import CommandHistoryEntry
# Color tokens live in panels_health.py (not theme.py) because
# panels_health already owns OK_FAIL_FLAGS -- the single source of truth
# this aggregate is derived from -- so importing both from one module
# means the flag list and its colors can never drift apart.
from .panels_health import GRAY, GREEN, OK_FAIL_FLAGS, RED
from .theme import mode_color, phase_color
from .widgets import StatusDot


# ── Top status strip ─────────────────────────────────────────────────────────
class TopStatusStrip(QWidget):
    """Single-line overview pinned at the top of the main window."""

    STALE_AMBER_S = 2.0
    STALE_RED_S = 5.0

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: #141414; border-bottom: 1px solid #2a2a2a;")
        lay = QHBoxLayout(self); lay.setContentsMargins(10, 4, 10, 4); lay.setSpacing(14)

        self._mode = QLabel("MODE: —");    self._mode.setStyleSheet("font-weight: bold; font-size: 11pt;")
        self._phase = QLabel("PHASE: —");  self._phase.setStyleSheet("font-size: 11pt;")
        self._link = QLabel("LINK: —");    self._link.setStyleSheet("font-family: monospace; font-size: 10pt;")
        self._sess = QLabel("sess: —");    self._sess.setStyleSheet("font-family: monospace; font-size: 10pt; color: #888;")
        self._seq  = QLabel("seq: —");     self._seq.setStyleSheet("font-family: monospace; font-size: 10pt; color: #888;")

        # `_link` is the live link-staleness readout ("LINK: STALE Δs") --
        # safety-relevant, same protected tier as MODE/PHASE/HEALTH below.
        # It gets NO shrink treatment: default size policy, natural
        # minimumSizeHint, never clipped or hidden.
        #
        # `_sess`/`_seq`/`_disc` are genuinely secondary/debug info
        # (already dimmed). They keep the default Preferred size policy
        # (NOT Ignored -- an Ignored widget sharing a QHBoxLayout with a
        # competing addStretch() gets driven to width 0 UNCONDITIONALLY,
        # not just under pressure, which made them invisible at every
        # window size, verified empirically). `setMinimumWidth(1)`
        # overrides QLabel's minimumSizeHint floor (normally = full text
        # width) with a 1px floor -- NOT 0: `QWidget.minimumSize()`
        # defaults to QSize(0, 0), so `setMinimumWidth(0)` is
        # indistinguishable from never having called it at all and the
        # layout silently falls back to the full-text-width floor again
        # (verified empirically -- 0 is a no-op, 1 is not, and visually
        # identical). With a real (if tiny) explicit minimum, the layout
        # gives these labels their full natural width whenever there's
        # room and only compresses them under genuine pressure (e.g. a
        # long IP:port discovery string at a narrow window width).
        for _lbl in (self._sess, self._seq):
            _lbl.setMinimumWidth(1)

        # Aggregate health dot: green only when every OK/FAIL flag present
        # in the current packet is OK; red if any is FAIL; gray before the
        # first packet or when a packet carries none of the known flags
        # (legacy replay) -- same "stay quiet" rule as the Health tab.
        health_label = QLabel("HEALTH:")
        health_label.setStyleSheet("font-family: monospace; font-size: 10pt; color: #888;")
        self._health_dot = StatusDot(10)
        self._health_dot.set_color(GRAY)
        self._health_dot.setToolTip("No health data")

        lay.addWidget(self._mode); lay.addWidget(self._phase)
        lay.addWidget(health_label); lay.addWidget(self._health_dot)
        lay.addStretch()
        lay.addWidget(self._sess); lay.addWidget(self._seq); lay.addWidget(self._link)

        self._last_packet_mono: float = 0.0
        self._timer = QTimer(self); self._timer.timeout.connect(self._refresh_link); self._timer.start(500)

        self._disc = QLabel("disc: —"); self._disc.setStyleSheet(
            "font-family: monospace; font-size: 10pt; color: #888;")
        self._disc.setMinimumWidth(1)
        lay.addWidget(self._disc)

    def set_discovery(self, text: str, color: str = "#888") -> None:
        self._disc.setText(text)
        self._disc.setStyleSheet(
            f"font-family: monospace; font-size: 10pt; color: {color};")

    def on_packet(self, pkt: TelemetryPacket) -> None:
        self._last_packet_mono = time.monotonic()
        self._mode.setText(f"MODE: {pkt.mode or '—'}")
        self._mode.setStyleSheet(f"font-weight: bold; font-size: 11pt; color: {mode_color(pkt.mode or '')};")
        self._phase.setText(f"PHASE: {pkt.phase}")
        self._phase.setStyleSheet(f"font-size: 11pt; color: {phase_color(pkt.phase)};")
        self._sess.setText(f"sess: {pkt.session_id[:8]}")
        self._seq.setText(f"seq: {pkt.seq}")
        color, tooltip = self._health_summary(pkt)
        self._health_dot.set_color(color)
        self._health_dot.setToolTip(tooltip)

    @staticmethod
    def _health_summary(pkt: TelemetryPacket) -> tuple[str, str]:
        # Green requires every one of the 14 OK/FAIL flags to be present
        # and OK -- a single surviving `<key>_OK` token in an otherwise
        # truncated/partial STATUS field must NOT paint the master dot
        # green while the other 13 subsystems are simply unreported.
        tokens = set(pkt.status.split("|")) if pkt.status else set()
        failing: list[str] = []
        unreported: list[str] = []
        for key, _label in OK_FAIL_FLAGS:
            if f"{key}_FAIL" in tokens:
                failing.append(key)
            elif f"{key}_OK" not in tokens:
                unreported.append(key)
        if failing:
            return RED, ", ".join(failing) + " failing"
        if not unreported:
            return GREEN, "All health flags OK"
        if len(unreported) == len(OK_FAIL_FLAGS):
            return GRAY, "no health flags reported"
        return GRAY, f"{len(unreported)} flags unreported: " + ", ".join(unreported)

    def health_color(self) -> str:
        """Current aggregate health dot color, as ``#rrggbb``. Test-only
        accessor."""
        return self._health_dot.color()

    def health_tooltip(self) -> str:
        """Current aggregate health dot tooltip text. Test-only
        accessor."""
        return self._health_dot.toolTip()

    def _refresh_link(self) -> None:
        if self._last_packet_mono <= 0.0:
            self._link.setText("LINK: waiting"); self._link.setStyleSheet("color: #888; font-family: monospace; font-size: 10pt;")
            return
        dt = time.monotonic() - self._last_packet_mono
        if dt < self.STALE_AMBER_S:
            color, tag = "#2ecc71", "OK"
        elif dt < self.STALE_RED_S:
            color, tag = "#f39c12", "stale"
        else:
            color, tag = "#e74c3c", "STALE"
        self._link.setText(f"LINK: {tag} Δ{dt:4.1f}s")
        self._link.setStyleSheet(f"color: {color}; font-family: monospace; font-size: 10pt;")


# ── Values panel (scrollable readout) ────────────────────────────────────────
class ValuesPanel(QScrollArea):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        inner = QWidget(); self.setWidget(inner)
        self._lay = QVBoxLayout(inner); self._lay.setContentsMargins(4, 4, 4, 4); self._lay.setSpacing(2)
        self._fields: dict[str, QLabel] = {}

        self._section("SESSION")
        self._row("phase", "phase"); self._row("mode", "mode"); self._row("seq", "seq")
        self._row("timestamp", "time UTC"); self._row("rtc_valid", "rtc_valid"); self._row("session_id", "session")

        self._section("ENVIRONMENT")
        for k, l in [("ambient_temp_c", "amb T °C"),
                     ("ambient_pressure_mbar", "pressure mbar"),
                     ("uv", "UV")]:
            self._row(k, l)

        self._section("SAMPLES")
        for i in range(8):
            self._row(f"sample_{i}", f"sample {i} °C")

        self._section("RESISTANCE")
        for i in range(8):
            self._row(f"resistance_{i}", f"R{i} Ω")

        self._section("MOTORS")
        for m in range(2):
            self._row(f"m{m}_state", f"M{m} pos / tgt")
            self._row(f"m{m}_cfg", f"M{m} Hz · µstep")
            self._row(f"m{m}_mode", f"M{m} mode")
            self._row(f"m{m}_src", f"M{m} src")

        # STATUS flags (raw bitfield, tri-state flags, and COMPONENT_STATE)
        # are no longer rendered here as text rows -- the Health tab
        # (panels_health.HealthPanel) is the single home for flag-state
        # display, with green/red/amber dots for every flag.

        self._lay.addStretch()

    def _section(self, title: str) -> None:
        lbl = QLabel(title)
        lbl.setStyleSheet("font-weight: bold; color: #aaa; font-size: 10pt; "
                          "border-bottom: 1px solid #333; margin-top: 6px;")
        self._lay.addWidget(lbl)

    def _row(self, key: str, label: str) -> None:
        w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0)
        name = QLabel(label); name.setMinimumWidth(90); name.setStyleSheet("color: #888; font-size: 11pt;")
        val = QLabel("—"); val.setStyleSheet("font-family: monospace; font-size: 11pt;")
        h.addWidget(name); h.addWidget(val); h.addStretch()
        self._lay.addWidget(w)
        self._fields[key] = val

    def on_packet(self, pkt: TelemetryPacket) -> None:
        def reading(key: str, value: float, precision: int) -> str:
            valid = pkt.sensor_valid.get(key, True)
            if valid and math.isfinite(value):
                return f"{value:.{precision}f}"
            age = pkt.sensor_age_ms.get(key, -1)
            if math.isfinite(value) and age >= 0:
                return f"{value:.{precision}f} stale {age / 1000.0:.1f}s"
            return "N/A"

        f = self._fields
        f["phase"].setText(pkt.phase)
        f["mode"].setText(pkt.mode or "—")
        f["seq"].setText(str(pkt.seq))
        f["timestamp"].setText(pkt.timestamp)
        f["rtc_valid"].setText("1" if pkt.rtc_valid else "0")
        f["session_id"].setText(pkt.session_id[:12])
        f["ambient_temp_c"].setText(reading("AT", pkt.ambient_temp_c, 2))
        f["ambient_pressure_mbar"].setText(
            reading("AP", pkt.ambient_pressure_mbar, 1))
        f["uv"].setText(reading("UV", pkt.uv, 3))
        for i in range(8):
            if i < len(pkt.sample_temps_c):
                f[f"sample_{i}"].setText(
                    reading(f"S{i}", pkt.sample_temps_c[i], 2))
            else:
                f[f"sample_{i}"].setText("N/A")
        # Sample-resistance rows. Only the two MAX31865-monitored specimens
        # (sensor.max31865_sample_indices) carry live ohms; every other slot
        # legitimately dashes. A literal '-' on the wire surfaces as None
        # here; show an em-dash so the operator knows it's an unmonitored
        # channel rather than a broken sensor.
        for i in range(8):
            if i < len(pkt.sample_resistance_ohm):
                v = pkt.sample_resistance_ohm[i]
                f[f"resistance_{i}"].setText("—" if v is None else f"{v:.2f}")
            else:
                f[f"resistance_{i}"].setText("—")
        # Two-motor rendering; missing motors show "—".
        for m in range(2):
            if m < len(pkt.steppers):
                mot = pkt.steppers[m]
                f[f"m{m}_state"].setText(f"{mot['position']} / {mot['target']}")
                f[f"m{m}_cfg"].setText(f"{mot['hz']:.0f}Hz · µ{mot['microstep']}")
                mode = ("MOVING" if mot['moving']
                        else ("HOLD" if mot['holding']
                              else ("ON" if mot['enabled'] else "OFF")))
                f[f"m{m}_mode"].setText(mode)
                f[f"m{m}_src"].setText(mot['source'] or "—")
            else:
                for k in ("state", "cfg", "mode", "src"):
                    f[f"m{m}_{k}"].setText("—")
        # Status flags (raw bitfield, heater inhibit, resistance OK/FAIL,
        # COMPONENT_STATE) are rendered exclusively by the Health tab now
        # -- see panels_health.HealthPanel.on_packet.


# ── Preflight checklist ───────────────────────────────────────────────────────
class PreflightPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 6, 6, 6); lay.setSpacing(4)
        title = QLabel("Preflight checklist")
        title.setStyleSheet("font-weight: bold; font-size: 11pt;")
        lay.addWidget(title)
        self._items: dict[str, tuple[StatusDot, QLabel]] = {}
        for key, label in [
            ("rtc",        "RTC reporting valid"),
            ("ambient",    "Ambient sensors in-range"),
            ("heaters",    "6 heater duties reporting"),
            ("stepper_en", "Motors enabled"),
            ("link",       "Telemetry link healthy"),
            ("uniformity", "Specimen uniformity OK"),
            ("overtemp",   "No over-temperature latch"),
        ]:
            row = QHBoxLayout(); w = QWidget(); w.setLayout(row); row.setContentsMargins(0, 0, 0, 0)
            dot = StatusDot(12); dot.set_color("#555")
            lbl = QLabel(label); lbl.setStyleSheet("font-size: 11pt;")
            # Word-wrap (rather than a fixed one-line width) lets this
            # panel shrink below the widest checklist phrase -- e.g. "6
            # heater duties reporting" was, at one point, the single
            # largest contributor to the right dock's minimum width,
            # forcing the whole window wider than 1280px even after the
            # dock's own minimum was relaxed.
            lbl.setWordWrap(True)
            row.addWidget(dot); row.addWidget(lbl, 1)
            lay.addWidget(w)
            self._items[key] = (dot, lbl)
        lay.addStretch()

    def on_packet(self, pkt: TelemetryPacket, link_ok: bool) -> None:
        def mark(key: str, good) -> None:
            """`good` is either a bool (green/red) or an explicit CSS
            color string, for checklist items with a tri-state result."""
            dot, _ = self._items[key]
            if isinstance(good, str):
                dot.set_color(good)
            else:
                dot.set_color("#2ecc71" if good else "#e74c3c")
        mark("rtc", bool(pkt.rtc_valid))
        mark("ambient", pkt.sensor_valid.get("AT", True) and
             pkt.sensor_valid.get("AP", True))
        mark("heaters", len(pkt.heater_duty) >= 6)
        # Dual-motor frames never populate the legacy `pkt.stepper` field, so
        # derive the checklist state from `pkt.steppers` instead: both
        # motors enabled is green, exactly one is amber, none (or no motor
        # telemetry at all) is red.
        n_enabled = sum(1 for m in pkt.steppers if m.get("enabled"))
        mark("stepper_en",
             "#2ecc71" if n_enabled == 2 else
             "#f39c12" if n_enabled == 1 else "#e74c3c")
        mark("link", link_ok)
        mark("uniformity", "UNIFORMITY_FAIL" not in pkt.status)
        mark("overtemp", "OVERTEMP_FAIL" not in pkt.status)

    def dot_color(self, key: str) -> str:
        """Current checklist dot color for `key` (e.g. "stepper_en"), as
        ``#rrggbb``. Test-only accessor."""
        return self._items[key][0].color()


# ── Command history ───────────────────────────────────────────────────────────
class CmdHistoryPanel(QWidget):
    reissue_requested = pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 6, 6, 6); lay.setSpacing(4)

        header = QHBoxLayout()
        header.addWidget(QLabel("Command history"))
        clear = QPushButton("Clear"); clear.clicked.connect(self._clear)
        header.addStretch(); header.addWidget(clear)
        lay.addLayout(header)

        self._list = QListWidget()
        self._list.setStyleSheet("font-family: monospace; font-size: 10pt;")
        self._list.setWordWrap(True)
        self._list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._list.setTextElideMode(Qt.TextElideMode.ElideNone)
        self._list.itemDoubleClicked.connect(self._on_reissue)
        lay.addWidget(self._list, 1)

    def append(self, entry: CommandHistoryEntry) -> None:
        mark = "✔" if entry.ok else "✖"
        color = "#2ecc71" if entry.ok else "#e74c3c"
        body = entry.response.body if entry.ok else entry.response.error or entry.response.raw
        item = QListWidgetItem(f"{entry.ts}  {mark}  {entry.command}   ({entry.latency_ms:5.0f} ms)   {body}")
        item.setForeground(QColor(color))
        item.setData(Qt.ItemDataRole.UserRole, entry.command)
        item.setToolTip(f"Raw: {entry.response.raw}")
        self._list.insertItem(0, item)

    def _clear(self) -> None:
        self._list.clear()

    def _on_reissue(self, item: QListWidgetItem) -> None:
        cmd = item.data(Qt.ItemDataRole.UserRole)
        if cmd:
            self.reissue_requested.emit(cmd)


# ── Log ──────────────────────────────────────────────────────────────────────
class LogPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 6, 6, 6); lay.setSpacing(4)

        row = QHBoxLayout()
        row.addWidget(QLabel("Event log"))
        row.addStretch()
        save = QPushButton("Save…"); save.clicked.connect(self._save)
        clear = QPushButton("Clear"); clear.clicked.connect(self._clear)
        row.addWidget(save); row.addWidget(clear)
        lay.addLayout(row)

        self._text = QTextEdit(); self._text.setReadOnly(True)
        self._text.setStyleSheet("font-family: monospace; font-size: 10pt; background: #0a0a0a; color: #cccccc;")
        lay.addWidget(self._text, 1)

    def append(self, line: str) -> None:
        ts = time.strftime("%H:%M:%S")
        self._text.append(f"[{ts}] {line}")

    def _save(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Save log", "coatheal_log.txt", "Text (*.txt)")
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self._text.toPlainText())
        except OSError as exc:
            self.append(f"[error] save failed: {exc}")

    def _clear(self) -> None:
        self._text.clear()


# ── Motor status dock (M0 + M1) ──────────────────────────────────────────────
class MotorPanel(QWidget):
    """Live state tiles for the two sample-bending motors.

    Reads `packet.steppers[0]` and `[1]`. Shows each motor's position,
    target, step rate, microstep, and enable/move indicators. This is a
    read-only dashboard panel — control still happens through the existing
    StepperPanel on the left dock.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        outer = QVBoxLayout(self); outer.setContentsMargins(6, 6, 6, 6); outer.setSpacing(6)
        self._motor_widgets: list[dict[str, QLabel | StatusDot]] = []
        for m in range(2):
            box = QGroupBox(f"Motor {m}")
            grid = QGridLayout(box); grid.setContentsMargins(8, 8, 8, 8); grid.setSpacing(4)

            header = QHBoxLayout()
            title = QLabel(f"M{m}")
            title.setStyleSheet("font-weight: bold; font-size: 12pt; color: #eee;")
            header.addWidget(title)
            en_dot = StatusDot(10); en_dot.set_color("#555")
            mv_dot = StatusDot(10); mv_dot.set_color("#555")
            hold_dot = StatusDot(10); hold_dot.set_color("#555")
            header.addStretch()
            header.addWidget(QLabel("en")); header.addWidget(en_dot)
            header.addWidget(QLabel("mv")); header.addWidget(mv_dot)
            header.addWidget(QLabel("hold")); header.addWidget(hold_dot)
            grid.addLayout(header, 0, 0, 1, 4)

            def _kv_row(row: int, left_key: str, left_label: str,
                         right_key: str, right_label: str) -> dict[str, QLabel]:
                l = QLabel(left_label); l.setStyleSheet("color: #888; font-size: 10pt;")
                lv = QLabel("—"); lv.setStyleSheet("font-family: monospace; font-size: 11pt;")
                r = QLabel(right_label); r.setStyleSheet("color: #888; font-size: 10pt;")
                rv = QLabel("—"); rv.setStyleSheet("font-family: monospace; font-size: 11pt;")
                grid.addWidget(l, row, 0); grid.addWidget(lv, row, 1)
                grid.addWidget(r, row, 2); grid.addWidget(rv, row, 3)
                return {left_key: lv, right_key: rv}

            cells: dict[str, QLabel | StatusDot] = {
                "en_dot": en_dot, "mv_dot": mv_dot, "hold_dot": hold_dot,
            }
            cells.update(_kv_row(1, "pos", "pos",      "tgt", "tgt"))
            cells.update(_kv_row(2, "hz",  "Hz",       "us",  "µstep"))
            cells.update(_kv_row(3, "hold_s", "hold s", "pulses", "pulses"))
            cells.update(_kv_row(4, "src", "src",      "motor_id", "motor_id"))
            self._motor_widgets.append(cells)
            outer.addWidget(box)
        outer.addStretch()

    def on_packet(self, pkt: TelemetryPacket) -> None:
        for m, cells in enumerate(self._motor_widgets):
            if m >= len(pkt.steppers):
                for key in ("pos", "tgt", "hz", "us", "hold_s", "pulses", "src", "motor_id"):
                    w = cells[key]; assert isinstance(w, QLabel); w.setText("—")
                cells["en_dot"].set_color("#555")
                cells["mv_dot"].set_color("#555")
                cells["hold_dot"].set_color("#555")
                continue
            mot = pkt.steppers[m]
            def _set(key: str, text: str) -> None:
                w = cells[key]
                assert isinstance(w, QLabel)
                w.setText(text)
            _set("pos",      str(mot["position"]))
            _set("tgt",      str(mot["target"]))
            _set("hz",       f"{mot['hz']:.0f}")
            _set("us",       str(mot["microstep"]))
            _set("hold_s",   f"{mot['hold_s']:.1f}")
            _set("pulses",   str(mot["pulses"]))
            _set("src",      mot["source"] or "—")
            _set("motor_id", str(mot["motor_id"]))
            cells["en_dot"].set_color("#2ecc71" if mot["enabled"] else "#7f8c8d")
            cells["mv_dot"].set_color("#f39c12" if mot["moving"] else "#333")
            cells["hold_dot"].set_color("#3498db" if mot["holding"] else "#333")


# ── Pull events log table ────────────────────────────────────────────────────
class PullEventsPanel(QWidget):
    """Scrolling table of `EVT,PULL,...` events.

    Populated by the `TelemetryReceiver.pull_event` signal. Most recent
    event goes on top. The table is append-only; a "Clear" button wipes
    the in-memory list but not the `<log>_pulls.csv` mirror.
    """

    COLUMNS = ("time", "motor", "pull_id", "steps", "hold s", "samples", "session")
    MAX_ROWS = 500

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 6, 6, 6); lay.setSpacing(4)

        header = QHBoxLayout()
        header.addWidget(QLabel("Pull events"))
        header.addStretch()
        clear = QPushButton("Clear"); clear.clicked.connect(self._clear)
        header.addWidget(clear)
        lay.addLayout(header)

        self._table = QTableWidget(0, len(self.COLUMNS))
        self._table.setHorizontalHeaderLabels(list(self.COLUMNS))
        self._table.verticalHeader().setVisible(False)
        self._table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self._table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self._table.setAlternatingRowColors(True)
        self._table.setStyleSheet("font-family: monospace; font-size: 10pt;")
        self._table.horizontalHeader().setStretchLastSection(True)
        lay.addWidget(self._table, 1)

    def on_pull_event(self, ev: PullEvent) -> None:
        ts = time.strftime("%H:%M:%S")
        samples = "|".join(str(s) for s in ev.samples) or "—"
        values = [
            ts,
            f"M{ev.motor_id}",
            str(ev.pull_id),
            str(ev.steps_moved),
            f"{ev.hold_s:.1f}",
            samples,
            ev.session_id[:8],
        ]
        self._table.insertRow(0)
        for col, val in enumerate(values):
            item = QTableWidgetItem(val)
            # Colour the motor column so M0 / M1 pop visually.
            if col == 1:
                item.setForeground(QColor("#2ecc71" if ev.motor_id == 0 else "#e67e22"))
            self._table.setItem(0, col, item)
        # Trim history.
        while self._table.rowCount() > self.MAX_ROWS:
            self._table.removeRow(self._table.rowCount() - 1)

    def _clear(self) -> None:
        self._table.setRowCount(0)
