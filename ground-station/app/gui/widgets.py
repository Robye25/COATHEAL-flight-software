"""Small shared widgets: toast overlay, status dot, confirm dialog."""
from __future__ import annotations

from typing import Optional

from PyQt6.QtCore import Qt, QTimer, QPoint
from PyQt6.QtGui import QPainter, QColor
from PyQt6.QtWidgets import QLabel, QMessageBox, QWidget


class StatusDot(QWidget):
    """Small colored circle used as an at-a-glance indicator."""

    def __init__(self, diameter: int = 10, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._d = diameter
        self._color = QColor("#555555")
        self.setFixedSize(diameter, diameter)

    def set_color(self, css: str) -> None:
        self._color = QColor(css)
        self.update()

    def paintEvent(self, _event) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(self._color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(0, 0, self._d - 1, self._d - 1)


class Toast(QLabel):
    """Fire-and-forget inline toast.

    Use `Toast.anchor(parent_widget, text, ok=True)` — the toast paints itself
    over the parent's top-right corner for ~2.5 s then deletes itself.
    """

    _DURATION_MS = 2500

    def __init__(self, text: str, ok: bool, parent: QWidget):
        super().__init__(text, parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        bg = "#27ae60" if ok else "#c0392b"
        self.setStyleSheet(
            f"background-color: {bg}; color: white; "
            f"padding: 4px 10px; border-radius: 4px; font-size: 11px;"
        )
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.adjustSize()
        geo = parent.rect()
        self.move(geo.right() - self.width() - 8, geo.top() + 4)
        self.show()
        self.raise_()
        QTimer.singleShot(self._DURATION_MS, self.deleteLater)

    @classmethod
    def anchor(cls, parent: QWidget, text: str, ok: bool = True) -> "Toast":
        return cls(text, ok, parent)


def confirm(parent: QWidget, title: str, body: str) -> bool:
    """Yes/No modal. Returns True iff the user picks Yes."""
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Warning)
    box.setWindowTitle(title)
    box.setText(body)
    box.setStandardButtons(QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
    box.setDefaultButton(QMessageBox.StandardButton.No)
    return box.exec() == QMessageBox.StandardButton.Yes
