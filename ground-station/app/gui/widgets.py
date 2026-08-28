"""Small shared widgets: status dot, buttons with semantic classes, gated
buttons, the persistent response line, segmented selector, confirm dialog.

No toasts: feedback is the persistent `ResponseLine` under each control
group plus the console (redesign spec §6.3).
"""
from __future__ import annotations

from typing import Callable, Iterable, List, Optional, Sequence, Tuple

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QPainter
from PyQt6.QtWidgets import (
    QButtonGroup, QFrame, QHBoxLayout, QLabel, QMessageBox, QPushButton,
    QSizePolicy, QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse

# Semantic button classes. Pick by what the action *does*:
#   panic   — unconfirmed emergency stop (HEATERS OFF, STOP MOTORS)
#   danger  — stops/kills motion, power, or the run (confirmed or not)
#   success — arm/start/run/resume actions that bring something to life
#   primary — the default action colour for ordinary command sends
#   neutral — read-only queries and low-stakes config writes/clears
BUTTON_CLASSES = {
    "primary": "#2980b9",
    "success": "#27ae60",
    "danger":  "#c0392b",
    "neutral": "#7f8c8d",
    "panic":   "#8e1d1d",
}
GREEN = "#2ecc71"
RED = "#e74c3c"
AMBER = "#f39c12"
GRAY = "#666666"
BLUE = "#3498db"
MUTED = "#888888"
MONO_CSS = "font-family: monospace;"


def style_button(btn: QPushButton, cls: str = "primary", *, bold: bool = True,
                 min_height: int = 26, bg: Optional[str] = None, fg: str = "white",
                 compact: bool = False) -> None:
    if bg is None:
        bg = BUTTON_CLASSES[cls]
    btn.setMinimumHeight(min_height)
    padding = "2px 4px" if compact else "3px 8px"
    btn.setStyleSheet(
        f"QPushButton {{ background: {bg}; color: {fg}; font-weight: {'bold' if bold else 'normal'}; "
        f"border: 1px solid #222; border-radius: 3px; padding: {padding}; }}"
        f"QPushButton:hover {{ background: #3a3a3a; }}"
        f"QPushButton:pressed {{ background: #111; }}"
        f"QPushButton:disabled {{ background: #1a1a1a; color: #555; }}"
    )


class GatedButton(QPushButton):
    """A command button that explains why it is disabled. `set_reason(None)`
    enables it; a string disables it and appends the reason to the tooltip
    (the base tooltip is the `Sends: ...` line)."""

    def __init__(self, label: str, cls: str = "primary", *, sends: str = "",
                 min_height: int = 26, bold: bool = True, compact: bool = False, parent=None):
        super().__init__(label, parent)
        self._base_tip = f"Sends: {sends}" if sends else ""
        self._reason: Optional[str] = None
        style_button(self, cls, bold=bold, min_height=min_height, compact=compact)
        self.setToolTip(self._base_tip)

    def set_sends_tip(self, text: str) -> None:
        """Replace the base `Sends: ...` line (kept across set_reason)."""
        self._base_tip = text
        self.set_reason(self._reason)

    def set_reason(self, reason: Optional[str]) -> None:
        self._reason = reason
        self.setEnabled(reason is None)
        tip = self._base_tip
        if reason:
            tip = (tip + "\n" if tip else "") + f"Disabled: {reason}"
        self.setToolTip(tip)

    def reason(self) -> Optional[str]:
        return self._reason


def make_button(label: str, cls: str = "primary", *, sends: str = "",
                slot: Optional[Callable[[], None]] = None, min_height: int = 26,
                bold: bool = True, compact: bool = False, width: Optional[int] = None) -> GatedButton:
    btn = GatedButton(label, cls, sends=sends, min_height=min_height, bold=bold, compact=compact)
    if width is not None:
        btn.setFixedWidth(width)
    if slot is not None:
        btn.clicked.connect(lambda _checked=False: slot())
    return btn


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

    def color(self) -> str:
        """Current fill color as ``#rrggbb`` — read accessor for tests."""
        return self._color.name()

    def paintEvent(self, _event) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(self._color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(0, 0, self._d - 1, self._d - 1)


class Indicator(QWidget):
    """`● label value` row. `set_color` paints the dot, `set_value` the
    monospace value at the right."""

    def __init__(self, label: str, *, dot: int = 10, value: str = "", parent=None):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)
        self._dot = StatusDot(dot)
        self._dot.set_color(GRAY)
        self._label = QLabel(label)
        self._value = QLabel(value)
        self._value.setStyleSheet(f"{MONO_CSS} color: {MUTED};")
        lay.addWidget(self._dot)
        lay.addWidget(self._label, 1)
        lay.addWidget(self._value)

    def set_color(self, css: str) -> None:
        self._dot.set_color(css)

    def color(self) -> str:
        return self._dot.color()

    def set_value(self, text: str, color: Optional[str] = None) -> None:
        self._value.setText(text)
        self._value.setStyleSheet(f"{MONO_CSS} color: {color or MUTED};")

    def value(self) -> str:
        return self._value.text()

    def set_label(self, text: str) -> None:
        self._label.setText(text)


class SectionLabel(QLabel):
    def __init__(self, text: str, parent=None):
        super().__init__(text, parent)
        self.setStyleSheet("font-weight: bold; color: #aaa; font-size: 10pt; "
                           "border-bottom: 1px solid #333; margin-top: 4px;")


class ResponseLine(QLabel):
    """Persistent last-response line under a control group (spec §6.3).

    Sticky until the next response for the same group. Green ACK with the
    reply body, red NACK with the reason, both with latency. Word-wraps so
    a long NACK reason is never clipped.
    """

    def __init__(self, parent=None):
        super().__init__("—", parent)
        self.setWordWrap(True)
        self.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._paint(MUTED)
        self.last_ok: Optional[bool] = None
        self.last_command: str = ""

    def _paint(self, color: str) -> None:
        self.setStyleSheet(
            f"{MONO_CSS} font-size: 9pt; color: {color}; background: #111; "
            "border: 1px solid #2a2a2a; border-radius: 3px; padding: 3px 6px;")

    def show_response(self, cmd: str, resp: CommandResponse, ms: float) -> None:
        glyph = "✔" if resp.ok else "✖"
        body = resp.body if resp.ok else (resp.error or resp.raw or "no reply")
        latency = f" · {ms:.0f} ms" if ms > 0 else ""
        self.setText(f"{glyph} {cmd.strip()}{latency}\n{body}" if body else f"{glyph} {cmd.strip()}{latency}")
        self._paint(GREEN if resp.ok else RED)
        self.last_ok = resp.ok
        self.last_command = cmd.strip()

    def show_note(self, text: str, color: str = MUTED) -> None:
        self.setText(text)
        self._paint(color)

    def clear_response(self) -> None:
        self.setText("—")
        self._paint(MUTED)
        self.last_ok = None
        self.last_command = ""


class Segmented(QWidget):
    """Mutually exclusive button row (e.g. motor M0 | M1)."""

    valueChanged = pyqtSignal(object)

    def __init__(self, options: Sequence[Tuple[str, object]], parent=None, *, current: int = 0):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        self._group = QButtonGroup(self)
        self._group.setExclusive(True)
        self._buttons: List[QPushButton] = []
        self._values: List[object] = []
        for index, (label, value) in enumerate(options):
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setMinimumHeight(26)
            btn.setStyleSheet(
                "QPushButton { background: #202020; color: #bbb; border: 1px solid #333; padding: 3px 12px; font-weight: bold; }"
                "QPushButton:checked { background: #2980b9; color: white; }"
                "QPushButton:hover:!checked { background: #2a2a2a; }")
            self._group.addButton(btn, index)
            lay.addWidget(btn)
            self._buttons.append(btn)
            self._values.append(value)
        if self._buttons:
            self._buttons[max(0, min(current, len(self._buttons) - 1))].setChecked(True)
        self._group.idClicked.connect(lambda idx: self.valueChanged.emit(self._values[idx]))

    def value(self) -> object:
        idx = self._group.checkedId()
        return self._values[idx] if 0 <= idx < len(self._values) else None

    def set_value(self, value: object) -> None:
        if value in self._values:
            self._buttons[self._values.index(value)].setChecked(True)


def confirm(parent: QWidget, title: str, body: str) -> bool:
    """Yes/No modal, default No. Returns True iff the user picks Yes."""
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Warning)
    box.setWindowTitle(title)
    box.setText(body)
    box.setStandardButtons(QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
    box.setDefaultButton(QMessageBox.StandardButton.No)
    return box.exec() == QMessageBox.StandardButton.Yes


def group_box(title: str) -> Tuple[QFrame, QVBoxLayout]:
    """A titled panel with the console's group styling. Returns (frame,
    inner layout)."""
    frame = QFrame()
    frame.setObjectName("group")
    frame.setStyleSheet("QFrame#group { background: #1b1b1b; border: 1px solid #2a2a2a; border-radius: 4px; }")
    outer = QVBoxLayout(frame)
    outer.setContentsMargins(6, 6, 6, 6)
    outer.setSpacing(5)
    title_lbl = QLabel(title.upper())
    title_lbl.setStyleSheet("color: #bbb; font-size: 8pt; font-weight: bold; letter-spacing: 1px; border: none;")
    title_lbl.setWordWrap(True)
    outer.addWidget(title_lbl)
    return frame, outer


def hrow(*widgets: QWidget, spacing: int = 6, stretch_last: bool = False) -> QWidget:
    w = QWidget()
    lay = QHBoxLayout(w)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(spacing)
    for widget in widgets:
        lay.addWidget(widget)
    if stretch_last:
        lay.addStretch()
    return w
