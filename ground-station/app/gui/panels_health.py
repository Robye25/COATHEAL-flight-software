"""Health tab — a status dot for every onboard OK/FAIL, tri-state, and
COMPONENT_STATE flag, always in the same place regardless of which flags a
given firmware build happens to send.

Wire authority for the OK/FAIL and tri-state flags:
    onboard/src/status_flags.cpp:9-25 (`ToStatusBitfield`) — exactly one of
    "<NAME>_OK" / "<NAME>_FAIL" per OK/FAIL entry, and exactly one of the
    tri-state pair, pipe-separated inside the STATUS= field.

Wire authority for COMPONENT_STATE:
    onboard/include/coatheal/component_health.hpp (state words: DISABLED,
    DISCOVERING, OK, DEGRADED, STALE, FAILED) and onboard/src/telemetry.cpp
    (which component keys are emitted). `app/protocol.py`'s
    `parse_telemetry_csv` turns COMPONENT_STATE=key:state|... into
    `TelemetryPacket.component_state: Dict[str, str]`.

This panel is the single home for flag-state display in the GUI; the
Values tab (panel_values.py) shows numbers only. `health_summary` is the
aggregate the top strip's HEALTH dot shows.
"""
from __future__ import annotations

from typing import Dict, List, Optional, Tuple

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QGridLayout, QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget,
)

from ..protocol import TelemetryPacket
from .widgets import StatusDot

GREEN = "#2ecc71"
RED = "#e74c3c"
AMBER = "#f39c12"
GRAY = "#666666"

# 14 OK/FAIL flags, in UI order (matches status_flags.cpp emission order
# minus the tri-state entries interleaved among them). Each entry is
# (wire key, human-readable label). Green when "<key>_OK" appears in
# STATUS=, red when "<key>_FAIL" appears, gray when neither does (legacy
# replays / not-yet-known firmware must stay visually quiet, never red).
OK_FAIL_FLAGS: List[Tuple[str, str]] = [
    ("SD",          "SD card"),
    ("USB",         "USB storage"),
    ("I2C",         "I2C bus"),
    ("SPI",         "SPI bus"),
    ("LINK",        "Radio link"),
    ("T_AMBIENT",   "Ambient temperature sensor"),
    ("P_AMBIENT",   "Ambient pressure sensor"),
    ("UNIFORMITY",  "Specimen uniformity"),
    ("OVERTEMP",    "Over-temperature latch clear"),
    ("ENERGY",      "Energy budget"),
    ("PWM",         "PWM driver"),
    ("STEPPER",     "Stepper motors"),
    ("SAMPLE_TEMP", "Sample temp (RTD HAT)"),
    ("RESISTANCE",  "Resistance (MAX31865)"),
]

# 3 tri-state flags: (amber token, green token, human-readable label).
# Amber token present => amber dot. Green token present => green dot.
# Neither present (legacy replay) => gray.
TRI_STATE_FLAGS: List[Tuple[str, str, str]] = [
    ("SIMULATED",        "REAL_SENSORS",  "Sensor source"),
    ("SEQ_PAUSED",       "SEQ_READY",     "Bend sequence"),
    ("HEATER_INHIBITED", "HEATER_ACTIVE", "Heater inhibit"),
]

# 6 COMPONENT_STATE entries: (wire key, human-readable label).
COMPONENTS: List[Tuple[str, str]] = [
    ("DPS310",      "DPS310 (pressure)"),
    ("ADS1115",     "ADS1115 (ADC)"),
    ("SEQUENT_RTD", "SEQUENT_RTD (RTD HAT)"),
    ("MOTOR0",      "Motor 0"),
    ("MOTOR1",      "Motor 1"),
    ("PWM",         "PWM controller"),
]

# Component-state color mapping. OK -> green (healthy). DEGRADED / STALE /
# FAILED -> red (degraded or error -- operator should look). DISABLED ->
# amber: a deliberately-disabled component is "non-nominal, not failing" --
# the same meaning amber already carries for the tri-state flags above --
# and must be visually distinct from "not reported" so an off-by-config
# mistake doesn't read as silence. DISCOVERING (boot-time, not yet
# resolved), any other unrecognized word, and an absent key all -> gray.
# This mirrors the OK/FAIL flags' "stay quiet on anything we don't
# positively know is bad" rule: an unrecognized future state word must not
# paint red.
_COMPONENT_RED = {"DEGRADED", "STALE", "FAILED"}
_COMPONENT_GREEN = {"OK"}
_COMPONENT_AMBER = {"DISABLED"}


def component_color(state: Optional[str]) -> str:
    if state in _COMPONENT_GREEN:
        return GREEN
    if state in _COMPONENT_RED:
        return RED
    if state in _COMPONENT_AMBER:
        return AMBER
    return GRAY


def health_summary(pkt: TelemetryPacket) -> Tuple[str, str]:
    """Aggregate (color, tooltip) for the top strip's HEALTH dot.

    Green only when every OK/FAIL flag is present and OK and no component
    is DEGRADED/STALE/FAILED; red if any flag is FAIL or any component is
    red; amber when everything else is green-worthy but sensors are
    simulated; gray before the first packet or when flags are unreported
    (a truncated STATUS field must not paint green over 13 silent flags).
    """
    tokens = set(pkt.status.split("|")) if pkt.status else set()
    failing: List[str] = []
    unreported: List[str] = []
    for key, _label in OK_FAIL_FLAGS:
        if f"{key}_FAIL" in tokens:
            failing.append(key)
        elif f"{key}_OK" not in tokens:
            unreported.append(key)
    component_state = pkt.component_state or {}
    red_components = [
        f"{key} {component_state[key]}" for key, _label in COMPONENTS
        if component_color(component_state.get(key)) == RED
    ]
    if failing or red_components:
        parts = []
        if failing:
            parts.append(", ".join(failing) + " failing")
        if red_components:
            parts.append(", ".join(red_components))
        return RED, "; ".join(parts)
    if not unreported:
        if "SIMULATED" in tokens:
            return AMBER, "running on simulated sensors"
        return GREEN, "All 14 system flags OK, components OK"
    if len(unreported) == len(OK_FAIL_FLAGS):
        return GRAY, "no health flags reported"
    return GRAY, f"{len(unreported)} flags unreported: " + ", ".join(unreported)


class HealthPanel(QWidget):
    """Health tab content. Reuses `StatusDot` from widgets.py for every
    indicator so tests can assert colors via the same `.color()`
    accessor pattern PreflightPanel established."""

    def __init__(self, parent=None):
        super().__init__(parent)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        inner = QWidget()
        scroll.setWidget(inner)
        outer.addWidget(scroll)

        lay = QVBoxLayout(inner)
        lay.setContentsMargins(8, 8, 18, 8)
        lay.setSpacing(8)

        # key -> (dot, label). Keys: OK/FAIL flags use their wire key
        # directly (e.g. "SD", "I2C"); tri-state flags use their amber
        # token (e.g. "SIMULATED", "SEQ_PAUSED"); COMPONENT_STATE entries
        # use "component_<key>" (e.g. "component_PWM") to avoid colliding
        # with the OK/FAIL "PWM" entry.
        self._items: Dict[str, Tuple[StatusDot, QLabel]] = {}
        self._state_labels: Dict[str, QLabel] = {}

        lay.addWidget(self._section_title("System flags"))
        lay.addLayout(self._build_flag_grid(
            [(key, label, f"{key}_OK / {key}_FAIL") for key, label in OK_FAIL_FLAGS]
        ))

        lay.addWidget(self._section_title("Modes"))
        lay.addLayout(self._build_flag_grid(
            [(amber_tok, label, f"{amber_tok} / {green_tok}")
             for amber_tok, green_tok, label in TRI_STATE_FLAGS]
        ))

        lay.addWidget(self._section_title("Components"))
        lay.addLayout(self._build_component_rows())

        lay.addStretch()

    @staticmethod
    def _section_title(text: str) -> QLabel:
        lbl = QLabel(text)
        lbl.setStyleSheet(
            "font-weight: bold; color: #aaa; font-size: 10pt; "
            "border-bottom: 1px solid #333; margin-top: 4px;"
        )
        return lbl

    def _build_flag_grid(self, rows: List[Tuple[str, str, str]]) -> QGridLayout:
        """`rows` is (key, label, tooltip). Fixed 2-column grid of
        dot+label pairs -- doesn't reflow, but never forces the dock
        wider than its current minimum."""
        grid = QGridLayout()
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(4)
        for i, (key, label, tooltip) in enumerate(rows):
            # Single column: the right column is ~300 px on the smallest
            # supported screen and the longest label does not fit twice.
            row, col = i, 0
            dot = StatusDot(11)
            dot.set_color(GRAY)
            dot.setToolTip(tooltip)
            text = QLabel(label)
            text.setStyleSheet("font-size: 11pt;")
            text.setToolTip(tooltip)
            text.setWordWrap(True)
            cell = QWidget()
            h = QHBoxLayout(cell)
            h.setContentsMargins(0, 0, 0, 0)
            h.setSpacing(6)
            h.addWidget(dot)
            h.addWidget(text)
            h.addStretch()
            grid.addWidget(cell, row, col)
            self._items[key] = (dot, text)
        return grid

    def _build_component_rows(self) -> QVBoxLayout:
        vlay = QVBoxLayout()
        vlay.setContentsMargins(0, 0, 0, 0)
        vlay.setSpacing(4)
        for key, label in COMPONENTS:
            item_key = f"component_{key}"
            tooltip = f"COMPONENT_STATE={key}:<state>"
            row = QWidget()
            h = QHBoxLayout(row)
            h.setContentsMargins(0, 0, 0, 0)
            h.setSpacing(6)
            dot = StatusDot(11)
            dot.set_color(GRAY)
            dot.setToolTip(tooltip)
            text = QLabel(label)
            text.setStyleSheet("font-size: 11pt;")
            text.setToolTip(tooltip)
            text.setWordWrap(True)
            state_lbl = QLabel("—")
            state_lbl.setStyleSheet(
                "font-family: monospace; font-size: 10pt; color: #888;")
            h.addWidget(dot)
            h.addWidget(text)
            h.addStretch()
            h.addWidget(state_lbl)
            vlay.addWidget(row)
            self._items[item_key] = (dot, text)
            self._state_labels[item_key] = state_lbl
        return vlay

    # ── packet handling ──
    def on_packet(self, pkt: TelemetryPacket) -> None:
        tokens = set(pkt.status.split("|")) if pkt.status else set()

        for key, _label in OK_FAIL_FLAGS:
            ok = f"{key}_OK" in tokens
            fail = f"{key}_FAIL" in tokens
            color = GREEN if ok else RED if fail else GRAY
            self._items[key][0].set_color(color)

        for amber_tok, green_tok, _label in TRI_STATE_FLAGS:
            if amber_tok in tokens:
                color = AMBER
            elif green_tok in tokens:
                color = GREEN
            else:
                color = GRAY
            self._items[amber_tok][0].set_color(color)

        for key, _label in COMPONENTS:
            state = pkt.component_state.get(key)
            item_key = f"component_{key}"
            self._items[item_key][0].set_color(component_color(state))
            self._state_labels[item_key].setText(state if state else "—")

    # ── test accessors ──
    def dot_color(self, key: str) -> str:
        """Current dot color for `key` (e.g. "SD", "SIMULATED",
        "component_PWM"), as ``#rrggbb``. Mirrors
        `PreflightPanel.dot_color`."""
        return self._items[key][0].color()

    def state_word(self, key: str) -> str:
        """Current state word shown next to a COMPONENT_STATE dot. `key`
        is e.g. "component_PWM". Test-only accessor."""
        return self._state_labels[key].text()
