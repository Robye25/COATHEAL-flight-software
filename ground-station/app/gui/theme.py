"""Shared colors, palette, and tunables."""
from __future__ import annotations

from PyQt6.QtGui import QColor, QPalette

MAX_POINTS = 1200                     # rolling plot window depth (~20 min at 1 Hz)

HEATER_COLORS = [
    "#e74c3c", "#e67e22", "#f1c40f", "#2ecc71", "#1abc9c",
    "#3498db", "#9b59b6", "#e91e63", "#00bcd4",
]
# Rev-B: 8 sample heaters (H0..H7) + 1 electronics box heater (BOX). 9 total.
HEATER_LABELS = [f"H{i}" for i in range(8)] + ["BOX"]

PHASE_COLORS = {
    # Rev-A phase strings (kept for replay of old logs).
    "ASCENT_HOLD":     "#3498db",
    "ACTIVATION_RAMP": "#f39c12",
    "FLOAT_HOLD":      "#2ecc71",
    "DESCENT_FLOOR":   "#9b59b6",
    # Rev-B placeholders (Agent A is reshaping phases — floor-only control,
    # no setpoint suffix).
    "BOOT":            "#7f8c8d",
    "ASCENT":          "#3498db",
    "FLOAT":           "#2ecc71",
    "DESCENT":         "#9b59b6",
    "LANDED":          "#34495e",
    "STOPPED":         "#e74c3c",
}
MODE_COLORS = {"STANDBY": "#7f8c8d", "RUN": "#2ecc71", "SAFE": "#e74c3c"}

# Temperature / pressure thresholds used by the plots. These mirror the onboard
# config defaults (phase.activation_target_c=70, heater.max_sample_temp_c=85,
# phase.uniformity_tolerance_c=2, transition.ascent_to_activation_mbar=100).
ACTIVATION_TARGET_C = 70.0
OVERTEMP_CUTOFF_C = 85.0
UNIFORMITY_BAND_C = 2.0
ACTIVATION_PRESSURE_MBAR = 100.0


def phase_color(phase: str) -> str:
    for key, color in PHASE_COLORS.items():
        if key in phase.upper():
            return color
    return "#aaaaaa"


def mode_color(mode: str) -> str:
    return MODE_COLORS.get(mode.upper(), "#7f8c8d")


GLOBAL_QSS = """
* { color: #dddddd; }
QWidget { background-color: #1c1c1c; color: #dddddd; selection-background-color: #2980b9; selection-color: #ffffff; }

QMainWindow, QDialog { background-color: #1c1c1c; }

QMenuBar { background: #151515; color: #dddddd; border-bottom: 1px solid #2a2a2a; }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #2980b9; color: #ffffff; }
QMenu { background: #181818; color: #dddddd; border: 1px solid #2a2a2a; }
QMenu::item:selected { background: #2980b9; color: #ffffff; }

QStatusBar { background: #141414; color: #cccccc; border-top: 1px solid #2a2a2a; }
QStatusBar::item { border: none; }

QToolTip { background: #202020; color: #eeeeee; border: 1px solid #3a3a3a; padding: 4px; }

QDockWidget { color: #dddddd; }
QDockWidget::title { background: #202020; padding: 4px; color: #dddddd; }

QGroupBox {
    background: #1b1b1b; border: 1px solid #2a2a2a; border-radius: 4px;
    margin-top: 12px; padding-top: 10px; color: #dddddd;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left;
    left: 8px; padding: 0 6px; color: #bbbbbb; background: #1b1b1b;
}

QTabWidget::pane { background: #141414; border: 1px solid #2a2a2a; top: -1px; }
QTabBar { background: #141414; }
QTabBar::tab {
    background: #202020; color: #bbbbbb;
    padding: 6px 12px; margin-right: 2px; border: 1px solid #2a2a2a;
    border-bottom: none; border-top-left-radius: 3px; border-top-right-radius: 3px;
}
QTabBar::tab:selected { background: #2980b9; color: #ffffff; }
QTabBar::tab:hover:!selected { background: #2a2a2a; color: #ffffff; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTextEdit, QPlainTextEdit {
    background: #111111; color: #eeeeee;
    border: 1px solid #333333; border-radius: 3px; padding: 3px 5px;
    selection-background-color: #2980b9; selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QTextEdit:focus, QPlainTextEdit:focus {
    border: 1px solid #2980b9;
}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background: #2a2a2a; border: 1px solid #333; width: 14px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: none; border-bottom: 4px solid #bbbbbb; width: 0; height: 0; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: none; border-top: 4px solid #bbbbbb; width: 0; height: 0; }
QComboBox::drop-down { background: #2a2a2a; border-left: 1px solid #333; width: 18px; }
QComboBox::down-arrow { image: none; border-top: 5px solid #bbbbbb; width: 0; height: 0; margin-right: 4px; }
QComboBox QAbstractItemView {
    background: #181818; color: #eeeeee; border: 1px solid #2a2a2a;
    selection-background-color: #2980b9; selection-color: #ffffff;
}

QListWidget, QTreeView, QTableView {
    background: #111111; color: #dddddd; border: 1px solid #2a2a2a;
    alternate-background-color: #161616;
}
QListWidget::item:selected, QTreeView::item:selected, QTableView::item:selected {
    background: #2980b9; color: #ffffff;
}
QHeaderView::section {
    background: #202020; color: #cccccc; padding: 4px;
    border: 1px solid #2a2a2a;
}

QScrollArea { background: #1c1c1c; border: none; }
QScrollBar:vertical { background: #151515; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #3a3a3a; border-radius: 3px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #4a4a4a; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { background: none; height: 0; }
QScrollBar:horizontal { background: #151515; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: #3a3a3a; border-radius: 3px; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #4a4a4a; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { background: none; width: 0; }

QProgressBar { background: #0e0e0e; border: 1px solid #2a2a2a; border-radius: 2px; text-align: center; color: #dddddd; }

QSlider::groove:horizontal { background: #0e0e0e; height: 5px; border-radius: 2px; }
QSlider::handle:horizontal { background: #2980b9; width: 14px; margin: -5px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: #3498db; }

QCheckBox, QRadioButton { color: #dddddd; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; }
QCheckBox::indicator:unchecked { background: #111; border: 1px solid #444; }
QCheckBox::indicator:checked { background: #2980b9; border: 1px solid #2980b9; }

QSplitter::handle { background: #2a2a2a; }
QSplitter::handle:horizontal { width: 4px; }
QSplitter::handle:vertical { height: 4px; }

QMessageBox { background: #1c1c1c; color: #dddddd; }
QMessageBox QLabel { color: #dddddd; }
QMessageBox QPushButton { background: #2b2b2b; color: #dddddd; border: 1px solid #333; padding: 4px 14px; border-radius: 3px; min-width: 70px; }
QMessageBox QPushButton:hover { background: #3a3a3a; }

QFileDialog { background: #1c1c1c; color: #dddddd; }
QFileDialog QLabel { color: #dddddd; }
"""


def apply_dark_palette(app) -> None:
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window,        QColor("#1c1c1c"))
    palette.setColor(QPalette.ColorRole.WindowText,    QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.Base,          QColor("#111111"))
    palette.setColor(QPalette.ColorRole.AlternateBase, QColor("#1f1f1f"))
    palette.setColor(QPalette.ColorRole.Text,          QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.Button,        QColor("#2b2b2b"))
    palette.setColor(QPalette.ColorRole.ButtonText,    QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.ToolTipBase,   QColor("#202020"))
    palette.setColor(QPalette.ColorRole.ToolTipText,   QColor("#eeeeee"))
    palette.setColor(QPalette.ColorRole.Highlight,     QColor("#2980b9"))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor("#ffffff"))
    palette.setColor(QPalette.ColorRole.PlaceholderText, QColor("#777777"))
    app.setStyle("Fusion")
    app.setPalette(palette)
    app.setStyleSheet(GLOBAL_QSS)
