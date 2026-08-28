"""UI scale (redesign spec §4): the application font size, persisted, with
Ctrl+= / Ctrl+- / Ctrl+0. Labels that pin a small point size (hints)
stay small; everything else follows."""
from __future__ import annotations

from PyQt6.QtCore import QSettings
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import QApplication

MIN_PT = 8
MAX_PT = 16
KEY = "ui/font_pt"


class UiScale:
    def __init__(self, app: QApplication, settings: QSettings):
        self._app = app
        self._settings = settings
        base = app.font().pointSize()
        self.default_pt = base if base > 0 else 10
        stored = settings.value(KEY)
        try:
            self.current_pt = int(stored) if stored is not None else self.default_pt
        except (TypeError, ValueError):
            self.current_pt = self.default_pt
        self.apply(self.current_pt, persist=False)

    def apply(self, pt: int, persist: bool = True) -> int:
        pt = max(MIN_PT, min(MAX_PT, int(pt)))
        font = QFont(self._app.font())
        font.setPointSize(pt)
        self._app.setFont(font)
        self.current_pt = pt
        if persist:
            self._settings.setValue(KEY, pt)
        return pt

    def zoom_in(self) -> int:
        return self.apply(self.current_pt + 1)

    def zoom_out(self) -> int:
        return self.apply(self.current_pt - 1)

    def reset(self) -> int:
        return self.apply(self.default_pt)

    @property
    def percent(self) -> int:
        return int(round(100.0 * self.current_pt / self.default_pt))
