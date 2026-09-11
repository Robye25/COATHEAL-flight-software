"""Bottom console: every command with its reply, re-issue on double click,
free entry with completion and history (redesign spec §5.8)."""
from __future__ import annotations

import time
from typing import List, Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QKeyEvent
from PyQt6.QtWidgets import (
    QAbstractItemView, QCompleter, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
    QPlainTextEdit, QPushButton, QSplitter, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

from ..protocol import KNOWN_COMMANDS, CommandResponse
from ..reply_format import pretty_kv_body
from .widgets import AMBER, GREEN, MONO_CSS, MUTED, RED, style_button

MAX_ROWS = 500


class CommandEntry(QLineEdit):
    """Line edit with ↑/↓ history."""

    submitted = pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._history: List[str] = []
        self._index = 0
        self.setPlaceholderText("command · Tab completes · ↑↓ history · Enter sends")
        self.setStyleSheet(f"{MONO_CSS}")
        completer = QCompleter(sorted(KNOWN_COMMANDS), self)
        completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        completer.setCompletionMode(QCompleter.CompletionMode.PopupCompletion)
        self.setCompleter(completer)
        self.returnPressed.connect(self._submit)

    def _submit(self) -> None:
        text = self.text().strip()
        if not text:
            return
        if not self._history or self._history[-1] != text:
            self._history.append(text)
        self._index = len(self._history)
        self.clear()
        self.submitted.emit(text)

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Up and self._history:
            self._index = max(0, self._index - 1)
            self.setText(self._history[self._index])
            return
        if event.key() == Qt.Key.Key_Down and self._history:
            self._index = min(len(self._history), self._index + 1)
            self.setText(self._history[self._index] if self._index < len(self._history) else "")
            return
        super().keyPressEvent(event)

    def history(self) -> List[str]:
        return list(self._history)


class ConsolePanel(QWidget):
    send_requested = pyqtSignal(str)

    COLUMNS = ("time", "command", "", "ms", "response")

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self); lay.setContentsMargins(6, 4, 6, 6); lay.setSpacing(4)
        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(list(self.COLUMNS))
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setStyleSheet(f"{MONO_CSS} font-size: 9pt;")
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Interactive)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(4, QHeaderView.ResizeMode.Stretch)
        self.table.setColumnWidth(1, 220)
        self.table.itemSelectionChanged.connect(self._on_selection)
        self.table.itemDoubleClicked.connect(self._on_double_click)
        self.details = QPlainTextEdit(); self.details.setReadOnly(True)
        self.details.setStyleSheet(f"{MONO_CSS} font-size: 9pt; background: #0a0a0a; color: #ccc;")
        self.details.setPlaceholderText("select a row to see the full reply")
        self.details.setMaximumHeight(110)
        split = QSplitter(Qt.Orientation.Vertical)
        split.setObjectName("consoleSplit")
        split.addWidget(self.table); split.addWidget(self.details)
        split.setStretchFactor(0, 4); split.setStretchFactor(1, 1)
        lay.addWidget(split, 1)
        row = QHBoxLayout(); row.setSpacing(6)
        prompt = QLabel(">"); prompt.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: #3498db;")
        self.entry = CommandEntry()
        self.entry.submitted.connect(self.send_requested.emit)
        self.btn_send = QPushButton("SEND"); style_button(self.btn_send, "primary", min_height=26)
        self.btn_send.clicked.connect(self.entry._submit)
        self.btn_clear = QPushButton("CLEAR"); style_button(self.btn_clear, "neutral", min_height=26, bold=False)
        self.btn_clear.clicked.connect(self.clear)
        row.addWidget(prompt); row.addWidget(self.entry, 1); row.addWidget(self.btn_send); row.addWidget(self.btn_clear)
        lay.addLayout(row)
        self.note = QLabel(""); self.note.setStyleSheet(f"color: {AMBER}; font-size: 8pt;"); self.note.hide()
        lay.addWidget(self.note)
        self._bodies: List[str] = []

    def focus_entry(self) -> None:
        self.entry.setFocus()
        self.entry.selectAll()

    def set_note(self, text: str) -> None:
        self.note.setText(text)
        self.note.setVisible(bool(text))

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, _tag) -> None:
        body = resp.body if resp.ok else (resp.error or resp.raw or "no reply")
        row = self.table.rowCount()
        self.table.insertRow(row)
        values = (time.strftime("%H:%M:%SZ", time.gmtime()), cmd.strip(), "✔" if resp.ok else "✖",
                  f"{ms:.0f}" if ms > 0 else "—", body.replace("\n", " "))
        color = QColor(GREEN if resp.ok else RED)
        for col, value in enumerate(values):
            item = QTableWidgetItem(value)
            if col in (2, 4):
                item.setForeground(color)
            elif col == 0:
                item.setForeground(QColor(MUTED))
            item.setData(Qt.ItemDataRole.UserRole, cmd.strip())
            self.table.setItem(row, col, item)
        self._bodies.append(pretty_kv_body(body) if body else "")
        while self.table.rowCount() > MAX_ROWS:
            self.table.removeRow(0)
            self._bodies.pop(0)
        self.table.scrollToBottom()

    def _on_selection(self) -> None:
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        index = rows[0].row()
        if 0 <= index < len(self._bodies):
            cmd = self.table.item(index, 1).text()
            self.details.setPlainText(f"{cmd}\n{self._bodies[index]}")

    def _on_double_click(self, item: QTableWidgetItem) -> None:
        cmd = item.data(Qt.ItemDataRole.UserRole)
        if cmd:
            self.entry.setText(cmd)
            self.entry.setFocus()

    def clear(self) -> None:
        self.table.setRowCount(0)
        self._bodies.clear()
        self.details.clear()

    def row_count(self) -> int:
        return self.table.rowCount()
