"""Right-dock + bottom-dock panels: top status strip, values, preflight,
command history, log."""
from __future__ import annotations

import time
from collections import deque
from typing import Deque

from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtWidgets import (
    QFileDialog, QGroupBox, QHBoxLayout, QLabel, QListWidget, QListWidgetItem,
    QPushButton, QScrollArea, QTextEdit, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, TelemetryPacket
from .dispatch import CommandHistoryEntry
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

        lay.addWidget(self._mode); lay.addWidget(self._phase); lay.addStretch()
        lay.addWidget(self._sess); lay.addWidget(self._seq); lay.addWidget(self._link)

        self._last_packet_mono: float = 0.0
        self._timer = QTimer(self); self._timer.timeout.connect(self._refresh_link); self._timer.start(500)

    def on_packet(self, pkt: TelemetryPacket) -> None:
        self._last_packet_mono = time.monotonic()
        self._mode.setText(f"MODE: {pkt.mode or '—'}")
        self._mode.setStyleSheet(f"font-weight: bold; font-size: 11pt; color: {mode_color(pkt.mode or '')};")
        self._phase.setText(f"PHASE: {pkt.phase}")
        self._phase.setStyleSheet(f"font-size: 11pt; color: {phase_color(pkt.phase)};")
        self._sess.setText(f"sess: {pkt.session_id[:8]}")
        self._seq.setText(f"seq: {pkt.seq}")

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
        for k, l in [("ambient_temp_c", "amb T °C"), ("ambient_pressure_mbar", "pressure mbar"),
                     ("ambient_humidity_pct", "humidity %"), ("uv", "UV"), ("box_temp_c", "box T °C")]:
            self._row(k, l)

        self._section("SAMPLES")
        for i in range(9):
            self._row(f"sample_{i}", f"sample {i} °C")

        self._section("STEPPER")
        for k, l in [("stepper_state", "pos / tgt"), ("stepper_cfg", "Hz · µstep"),
                     ("stepper_mode", "mode"), ("stepper_src", "src")]:
            self._row(k, l)

        self._section("STATUS")
        self._row("status", "flags")

        self._lay.addStretch()

    def _section(self, title: str) -> None:
        lbl = QLabel(title)
        lbl.setStyleSheet("font-weight: bold; color: #aaa; font-size: 10px; "
                          "border-bottom: 1px solid #333; margin-top: 6px;")
        self._lay.addWidget(lbl)

    def _row(self, key: str, label: str) -> None:
        w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0)
        name = QLabel(label); name.setMinimumWidth(90); name.setStyleSheet("color: #888; font-size: 11px;")
        val = QLabel("—"); val.setStyleSheet("font-family: monospace; font-size: 11px;")
        h.addWidget(name); h.addWidget(val); h.addStretch()
        self._lay.addWidget(w)
        self._fields[key] = val

    def on_packet(self, pkt: TelemetryPacket) -> None:
        f = self._fields
        f["phase"].setText(pkt.phase)
        f["mode"].setText(pkt.mode or "—")
        f["seq"].setText(str(pkt.seq))
        f["timestamp"].setText(pkt.timestamp)
        f["rtc_valid"].setText("1" if pkt.rtc_valid else "0")
        f["session_id"].setText(pkt.session_id[:12])
        f["ambient_temp_c"].setText(f"{pkt.ambient_temp_c:.2f}")
        f["ambient_pressure_mbar"].setText(f"{pkt.ambient_pressure_mbar:.1f}")
        f["ambient_humidity_pct"].setText(f"{pkt.ambient_humidity_pct:.1f}")
        f["uv"].setText(f"{pkt.uv:.3f}")
        f["box_temp_c"].setText(f"{pkt.box_temp_c:.2f}")
        for i in range(9):
            if i < len(pkt.sample_temps_c):
                f[f"sample_{i}"].setText(f"{pkt.sample_temps_c[i]:.2f}")
        s = pkt.stepper
        if s is not None:
            f["stepper_state"].setText(f"{s.position} / {s.target}")
            f["stepper_cfg"].setText(f"{s.hz:.0f}Hz · µ{s.microstep}")
            mode = "MOVING" if s.moving else ("HOLD" if s.holding else ("ON" if s.enabled else "OFF"))
            f["stepper_mode"].setText(mode)
            f["stepper_src"].setText(s.source or "—")
        f["status"].setText(pkt.status)


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
            ("heaters",    "10 heater duties reporting"),
            ("stepper_en", "Stepper enabled"),
            ("link",       "Telemetry link healthy"),
            ("uniformity", "Specimen uniformity OK"),
            ("overtemp",   "No over-temperature latch"),
        ]:
            row = QHBoxLayout(); w = QWidget(); w.setLayout(row); row.setContentsMargins(0, 0, 0, 0)
            dot = StatusDot(12); dot.set_color("#555")
            lbl = QLabel(label); lbl.setStyleSheet("font-size: 11pt;")
            row.addWidget(dot); row.addWidget(lbl); row.addStretch()
            lay.addWidget(w)
            self._items[key] = (dot, lbl)
        lay.addStretch()

    def on_packet(self, pkt: TelemetryPacket, link_ok: bool) -> None:
        def mark(key: str, good: bool) -> None:
            dot, _ = self._items[key]
            dot.set_color("#2ecc71" if good else "#e74c3c")
        mark("rtc", bool(pkt.rtc_valid))
        mark("ambient", "T_AMBIENT_FAIL" not in pkt.status and "P_AMBIENT_FAIL" not in pkt.status)
        mark("heaters", len(pkt.heater_duty) >= 10)
        mark("stepper_en", pkt.stepper is not None and pkt.stepper.enabled)
        mark("link", link_ok)
        mark("uniformity", "UNIFORMITY_FAIL" not in pkt.status)
        mark("overtemp", "OVERTEMP_FAIL" not in pkt.status)


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
        self._list.itemDoubleClicked.connect(self._on_reissue)
        lay.addWidget(self._list, 1)

    def append(self, entry: CommandHistoryEntry) -> None:
        mark = "✔" if entry.ok else "✖"
        color = "#2ecc71" if entry.ok else "#e74c3c"
        body = entry.response.body if entry.ok else entry.response.error or entry.response.raw
        item = QListWidgetItem(f"{entry.ts}  {mark}  {entry.command}   ({entry.latency_ms:5.0f} ms)   {body}")
        item.setForeground(Qt.GlobalColor.white)
        item.setData(Qt.ItemDataRole.UserRole, entry.command)
        item.setToolTip(f"Raw: {entry.response.raw}")
        from PyQt6.QtGui import QColor
        item.setForeground(QColor(color))
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
