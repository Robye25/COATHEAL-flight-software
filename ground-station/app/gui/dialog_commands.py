"""The command reference (Help → Command reference, F2, the console's
COMMANDS button): every command the onboard accepts, its arguments, what it
does and where the console sends it from, searchable; a double-click puts
the command into the console entry. The content is protocol.COMMAND_REFERENCE,
which a test keeps equal to the onboard's own command table."""
from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractItemView, QDialog, QHeaderView, QLabel, QLineEdit, QTableWidget, QTableWidgetItem, QVBoxLayout,
)

from ..protocol import COMMAND_REFERENCE
from .widgets import MONO_CSS, MUTED

COLUMNS = ("Group", "Command", "Arguments", "What it does", "Sent from")


class CommandReferenceDialog(QDialog):
    insert_requested = pyqtSignal(str)   # "<COMMAND> " for the console entry

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Command reference")
        self.setModal(False)
        self.resize(980, 620)
        lay = QVBoxLayout(self)
        note = QLabel("Every command the onboard accepts (the same table as docs/protocol.md). "
                      "Type to filter; double-click a row to put the command into the console.")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.filter = QLineEdit(); self.filter.setPlaceholderText("filter (command, argument, description…)")
        self.filter.textChanged.connect(self._apply_filter)
        lay.addWidget(self.filter)
        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setWordWrap(True)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        for spec in COMMAND_REFERENCE:
            row = self.table.rowCount()
            self.table.insertRow(row)
            cells = (spec.group, spec.name, spec.args or "none", spec.summary, spec.where)
            for col, text in enumerate(cells):
                item = QTableWidgetItem(text)
                if col in (1, 2):
                    item.setFont(self.table.font())
                    item.setToolTip(text)
                self.table.setItem(row, col, item)
            self.table.item(row, 1).setData(Qt.ItemDataRole.UserRole, spec.name)
        self.table.resizeRowsToContents()
        self.table.cellDoubleClicked.connect(lambda row, _col: self.insert_row(row))
        self.table.setStyleSheet(f"QTableWidget {{ {MONO_CSS} }}")
        lay.addWidget(self.table, 1)
        self.filter.setFocus()

    # -- behaviour -----------------------------------------------------------------
    def _apply_filter(self, text: str) -> None:
        needle = text.strip().lower()
        for row in range(self.table.rowCount()):
            haystack = " ".join(self.table.item(row, col).text() for col in range(len(COLUMNS))).lower()
            self.table.setRowHidden(row, bool(needle) and needle not in haystack)

    def visible_commands(self) -> list:
        return [self.table.item(row, 1).text() for row in range(self.table.rowCount())
                if not self.table.isRowHidden(row)]

    def insert_row(self, row: int) -> None:
        item = self.table.item(row, 1)
        if item is not None:
            self.insert_requested.emit(f"{item.data(Qt.ItemDataRole.UserRole)} ")
