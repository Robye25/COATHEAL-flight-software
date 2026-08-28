"""Right column — live go/no-go checklist with a RUN CHECK button
(redesign spec §5.7)."""
from __future__ import annotations

from typing import Dict, Optional

from PyQt6.QtWidgets import QLabel, QScrollArea, QVBoxLayout, QWidget

from ..protocol import CommandResponse
from ..reply_format import parse_kv_body
from .checkout import checkout_items
from .dispatch import CommandDispatcher
from .state import OnboardState
from .widgets import AMBER, GRAY, GREEN, MONO_CSS, MUTED, RED, Indicator, ResponseLine, make_button, soft_breaks

_COLORS = {"green": GREEN, "amber": AMBER, "red": RED, "gray": GRAY}


class CheckoutPanel(QScrollArea):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self._rows: Dict[str, Indicator] = {}
        self.last_check: Optional[Dict[str, str]] = None
        inner = QWidget(); self.setWidget(inner)
        self._lay = QVBoxLayout(inner); self._lay.setContentsMargins(8, 8, 8, 8); self._lay.setSpacing(4)
        title = QLabel("Go / no-go (live)")
        title.setStyleSheet("font-weight: bold; color: #aaa; border-bottom: 1px solid #333;")
        self._lay.addWidget(title)
        self._rows_box = QWidget(); self._rows_lay = QVBoxLayout(self._rows_box)
        self._rows_lay.setContentsMargins(0, 0, 0, 0); self._rows_lay.setSpacing(4)
        self._lay.addWidget(self._rows_box)
        self.btn_check = make_button("RUN CHECK ALL", "primary", sends="CHECK", min_height=26,
                                     slot=lambda: self._disp.send("CHECK", tag=self))
        self._lay.addWidget(self.btn_check)
        self.summary = QLabel("last CHECK: —")
        self.summary.setWordWrap(True); self.summary.setMinimumWidth(1)
        self.summary.setStyleSheet(f"{MONO_CSS} color: {MUTED}; font-size: 8pt;")
        self._lay.addWidget(self.summary)
        self.resp = ResponseLine()
        self._lay.addWidget(self.resp)
        self._lay.addStretch()

    def update_state(self, state: OnboardState, *, link_ok: bool, unacked_alarms: int) -> None:
        items = checkout_items(state, link_ok=link_ok, unacked_alarms=unacked_alarms, last_check=self.last_check)
        seen = set()
        for item in items:
            seen.add(item.key)
            row = self._rows.get(item.key)
            if row is None:
                row = Indicator(item.label)
                self._rows[item.key] = row
                self._rows_lay.addWidget(row)
            row.set_label(item.label)
            row.set_color(_COLORS.get(item.color, GRAY))
            row.set_value(item.note, _COLORS.get(item.color, MUTED) if item.color != "green" else MUTED)
            row.setVisible(True)
        for key, row in self._rows.items():
            if key not in seen:
                row.setVisible(False)
        self.btn_check.set_reason("radio silence active" if state.silence else None)

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        if verb != "CHECK":
            return
        if resp.ok:
            self.last_check = parse_kv_body(resp.body)
            failing = [k for k, v in self.last_check.items() if v == "FAIL" and k != "overall"]
            errors = [f"{k}={v}" for k, v in self.last_check.items() if k.endswith("_error") and v not in ("NONE", "SKIPPED", "")]
            text = f"last CHECK: overall={self.last_check.get('overall', '?')}"
            if failing:
                text += " · FAIL: " + " ".join(failing)
            if errors:
                text += " · " + " ".join(errors)
            self.summary.setText(soft_breaks(text))
            self.summary.setStyleSheet(f"{MONO_CSS} font-size: 8pt; color: "
                                       f"{GREEN if self.last_check.get('overall') == 'OK' else RED};")
        if tag is self:
            self.resp.show_response(cmd, resp, ms)

    def row_color(self, key: str) -> Optional[str]:
        row = self._rows.get(key)
        return row.color() if row is not None else None
