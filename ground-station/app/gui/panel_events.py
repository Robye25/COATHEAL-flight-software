"""Bottom region — event log with level colouring and a filter, and the
EVT,PULL table (redesign spec §5.8)."""
from __future__ import annotations

import html
import time
from typing import List, Optional, Tuple

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import (
    QAbstractItemView, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QPushButton, QTableWidget,
    QTableWidgetItem, QTextEdit, QVBoxLayout, QWidget,
)

from ..protocol import PullEvent
from .widgets import AMBER, GREEN, MONO_CSS, MUTED, RED, style_button

MAX_LINES = 5000
_LEVEL_COLORS = {"ERROR": RED, "WARN": AMBER, "INFO": "#cccccc"}


def classify(line: str) -> str:
    lower = line.lower()
    if "[error]" in lower or "fatal" in lower or "nack" in lower or "failed" in lower:
        return "ERROR"
    if "[alarm]" in lower or "stale" in lower or "blocked" in lower or "warn" in lower or "dropped" in lower:
        return "WARN"
    return "INFO"


class EventsPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 4, 6, 6); lay.setSpacing(4)
        head = QHBoxLayout(); head.setSpacing(6)
        self.filter = QLineEdit(); self.filter.setPlaceholderText("filter (substring)")
        self.filter.textChanged.connect(self._rebuild)
        self.levels = QLabel("INFO WARN ERROR"); self.levels.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        self.btn_clear = QPushButton("CLEAR"); style_button(self.btn_clear, "neutral", min_height=22, bold=False)
        self.btn_clear.clicked.connect(self.clear)
        head.addWidget(self.filter, 1); head.addWidget(self.levels); head.addWidget(self.btn_clear)
        lay.addLayout(head)
        self.text = QTextEdit(); self.text.setReadOnly(True)
        self.text.setStyleSheet(f"{MONO_CSS} font-size: 9pt; background: #0a0a0a; color: #cccccc;")
        lay.addWidget(self.text, 1)
        self._lines: List[Tuple[str, str, str]] = []   # (ts, level, message)
        self._sink = None

    def set_sink(self, sink) -> None:
        """`sink(level, message)` — the session's events.log."""
        self._sink = sink

    def append(self, line: str, level: Optional[str] = None) -> None:
        level = level or classify(line)
        ts = time.strftime("%H:%M:%SZ", time.gmtime())
        self._lines.append((ts, level, line))
        if len(self._lines) > MAX_LINES:
            # Log FIRST: the early return must never cost the flight record
            # an event, only the incremental on-screen append.
            if self._sink is not None:
                self._sink(level, line)
            del self._lines[:500]  # trim in batches; a per-line rebuild is O(n) per event
            self._rebuild()
            return
        if self._matches(line):
            self.text.append(self._render(ts, level, line))
        if self._sink is not None:
            try:
                self._sink(level, line)
            except Exception:
                pass

    def _matches(self, line: str) -> bool:
        needle = self.filter.text().strip().lower()
        return not needle or needle in line.lower()

    @staticmethod
    def _render(ts: str, level: str, line: str) -> str:
        color = _LEVEL_COLORS.get(level, "#cccccc")
        return (f'<span style="color:{MUTED}">{ts}</span> '
                f'<span style="color:{color};font-weight:bold">{level:<5}</span> '
                f'<span style="color:{color}">{html.escape(line)}</span>')

    def _rebuild(self) -> None:
        self.text.clear()
        self.text.setHtml("<br>".join(self._render(ts, lvl, ln) for ts, lvl, ln in self._lines if self._matches(ln)))
        self.text.verticalScrollBar().setValue(self.text.verticalScrollBar().maximum())

    def clear(self) -> None:
        self._lines.clear()
        self.text.clear()

    def lines(self) -> List[str]:
        return [ln for _ts, _lvl, ln in self._lines]


class PullsPanel(QWidget):
    COLUMNS = ("time", "motor", "pull", "moved mm", "µsteps", "hold s", "samples", "session")
    MAX_ROWS = 500

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 4, 6, 6); lay.setSpacing(4)
        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(list(self.COLUMNS))
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setAlternatingRowColors(True)
        self.table.setStyleSheet(f"{MONO_CSS} font-size: 9pt;")
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setStretchLastSection(True)
        lay.addWidget(self.table, 1)

    def on_pull_event(self, ev: PullEvent, microstep: int = 4) -> None:
        # mm from µsteps at the 2 mm ball-screw lead; the caller passes the
        # motor's live divisor (default = the µ4 commissioning setting).
        mm = ev.steps_moved / (200.0 * max(1, microstep) / 2.0)
        values = [time.strftime("%H:%M:%SZ", time.gmtime()), f"M{ev.motor_id}", str(ev.pull_id),
                  f"{mm:+.2f}", f"{ev.steps_moved:+d}",
                  f"{ev.hold_s:.1f}", "|".join(str(s) for s in ev.samples) or "—", ev.session_id[-12:]]
        self.table.insertRow(0)
        for col, value in enumerate(values):
            item = QTableWidgetItem(value)
            if col == 1:
                item.setForeground(QColor("#2ecc71" if ev.motor_id == 0 else "#e67e22"))
            self.table.setItem(0, col, item)
        while self.table.rowCount() > self.MAX_ROWS:
            self.table.removeRow(self.table.rowCount() - 1)

    def row_count(self) -> int:
        return self.table.rowCount()
