"""Shared colors, palette, and tunables."""
from __future__ import annotations

from PyQt6.QtGui import QColor, QPalette

MAX_POINTS = 1200                     # rolling plot window depth (~20 min at 1 Hz)

HEATER_COLORS = [
    "#e74c3c", "#e67e22", "#f1c40f", "#2ecc71", "#1abc9c",
    "#3498db", "#9b59b6", "#e91e63", "#00bcd4", "#ff6b35",
]
HEATER_LABELS = [f"H{i}" for i in range(9)] + ["BOX"]

PHASE_COLORS = {
    "ASCENT_HOLD":     "#3498db",
    "ACTIVATION_RAMP": "#f39c12",
    "FLOAT_HOLD":      "#2ecc71",
    "DESCENT_FLOOR":   "#9b59b6",
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


def apply_dark_palette(app) -> None:
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window,        QColor("#1c1c1c"))
    palette.setColor(QPalette.ColorRole.WindowText,    QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.Base,          QColor("#111111"))
    palette.setColor(QPalette.ColorRole.AlternateBase, QColor("#1f1f1f"))
    palette.setColor(QPalette.ColorRole.Text,          QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.Button,        QColor("#2b2b2b"))
    palette.setColor(QPalette.ColorRole.ButtonText,    QColor("#dddddd"))
    palette.setColor(QPalette.ColorRole.Highlight,     QColor("#2980b9"))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor("#ffffff"))
    app.setPalette(palette)
