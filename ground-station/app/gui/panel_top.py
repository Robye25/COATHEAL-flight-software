"""Top strip (mode/phase/link/rate/target/session/T+/UTC/radio + panic
group) and the alarm strip (redesign spec §5.1, §5.2)."""
from __future__ import annotations

import time
from collections import deque
from datetime import datetime, timezone
from typing import Deque, List, Optional

from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QFontMetrics
from PyQt6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QSizePolicy, QWidget

from ..protocol import TelemetryPacket
from .alarms import Alarm
from .dispatch import CommandDispatcher
from .panels_health import health_summary
from ..session_dir import session_epoch
from .series_store import format_elapsed
from .state import OnboardState
from .theme import mode_color, phase_color
from .widgets import (
    AMBER, GRAY, GREEN, MONO_CSS, MUTED, RED, StatusDot, confirm, make_button, style_button,
)

STEPPER_MOTOR_IDS = (0, 1)
SILENCE_BG = "#2b1f45"
STRIP_BG = "#141414"


def _field(label: str) -> tuple[QWidget, QLabel]:
    box = QWidget()
    lay = QHBoxLayout(box)
    lay.setContentsMargins(8, 0, 8, 0)
    lay.setSpacing(5)
    name = QLabel(label)
    name.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
    value = QLabel("—")
    value.setStyleSheet(f"{MONO_CSS} font-weight: bold;")
    value.setMinimumWidth(1)
    lay.addWidget(name)
    lay.addWidget(value)
    box.setStyleSheet("border-right: 1px solid #333;")
    name.setStyleSheet(f"color: {MUTED}; font-size: 8pt; border: none;")
    value.setStyleSheet(f"{MONO_CSS} font-weight: bold; border: none;")
    return box, value


class TopStrip(QWidget):
    STALE_AMBER_S = 2.0
    STALE_RED_S = 5.0
    RATE_WINDOW_S = 5.0

    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self.setObjectName("topStrip")
        self._disp = dispatcher
        self._last_rx_mono: Optional[float] = None
        self._rx_times: Deque[float] = deque()
        self._session = ""
        self._t0_mono: Optional[float] = None
        self._t0_wall: Optional[float] = None   # onboard boot epoch from the session id
        self._silence_since: Optional[float] = None
        self._receiver_state = "idle"

        lay = QHBoxLayout(self)
        lay.setContentsMargins(6, 4, 6, 4)
        lay.setSpacing(0)

        self._mode = QLabel("—")
        self._mode.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._mode.setMinimumWidth(80)
        self._paint_mode("", GRAY)
        mode_box = QWidget(); ml = QHBoxLayout(mode_box); ml.setContentsMargins(4, 0, 8, 0); ml.setSpacing(5)
        mode_name = QLabel("MODE"); mode_name.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        ml.addWidget(mode_name); ml.addWidget(self._mode)
        lay.addWidget(mode_box)

        self._phase_box, self._phase = _field("PHASE")
        self._health_box, self._health = _field("HEALTH")
        self._health_dot = StatusDot(10); self._health_dot.set_color(GRAY)
        self._health_dot.setToolTip("No health data")
        self._health_box.layout().insertWidget(1, self._health_dot)
        self._health.hide()
        self._link_box, self._link = _field("LINK")
        self._link_dot = StatusDot(9); self._link_dot.set_color(GRAY)
        self._link_box.layout().insertWidget(1, self._link_dot)
        self._rx_box, self._rx = _field("RX")
        self._target_box, self._target = _field("TARGET")
        self._sess_box, self._sess = _field("SESSION")
        self._tplus_box, self._tplus = _field("T+")
        self._utc_box, self._utc = _field("UTC")
        self._replay_box, self._replay = _field("REPLAY")
        self._replay_box.hide()
        self._radio_box, self._radio = _field("RADIO")
        self._radio_dot = StatusDot(9); self._radio_dot.set_color(GRAY)
        self._radio_box.layout().insertWidget(1, self._radio_dot)
        self._radio_box.setStyleSheet("border: none;")
        # TARGET and SESSION absorb whatever width is left (and elide when
        # squeezed); everything else keeps its natural width so the
        # safety-relevant readouts are never clipped.
        self._target_full = "—"
        self._sess_full = "—"
        for label in (self._target, self._sess):
            label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
            label.setMinimumWidth(96)
            label.setMaximumWidth(360)
        for box in (self._phase_box, self._health_box, self._link_box, self._rx_box):
            lay.addWidget(box)
        lay.addWidget(self._target_box, 3)
        lay.addWidget(self._sess_box, 2)
        for box in (self._tplus_box, self._utc_box, self._replay_box, self._radio_box):
            lay.addWidget(box)

        # Panic group: unconfirmed HEATERS OFF / STOP MOTORS, confirmed ENTER SAFE.
        self.btn_heaters_off = make_button("HEATERS OFF", "panic", sends="HEATERS_OFF", min_height=32,
                                           slot=self.heaters_off)
        self.btn_stop_motors = make_button("STOP MOTORS", "panic", min_height=32, slot=self.stop_motors)
        self.btn_stop_motors.set_sends_tip("Sends: STEPPER_STOP 0\nSends: STEPPER_STOP 1")
        self.btn_enter_safe = make_button("ENTER SAFE", "danger", sends="ENTER_SAFE", min_height=32,
                                          slot=self.enter_safe)
        for btn in (self.btn_heaters_off, self.btn_stop_motors, self.btn_enter_safe):
            btn.setMinimumWidth(96)
            lay.addWidget(btn)
            lay.addSpacing(6)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(500)
        self._paint_strip(False)

    # -- panic actions ---------------------------------------------------------
    def heaters_off(self) -> None:
        self._disp.send("HEATERS_OFF", tag=self)

    def stop_motors(self) -> None:
        for motor_id in STEPPER_MOTOR_IDS:
            self._disp.send(f"STEPPER_STOP {motor_id}", tag=self)

    def enter_safe(self) -> None:
        if confirm(self, "Enter SAFE mode?", "Send ENTER_SAFE? Heaters off, motors stopped, logs synced."):
            self._disp.send("ENTER_SAFE", tag=self)

    # -- painting ---------------------------------------------------------------
    def _paint_mode(self, text: str, color: str) -> None:
        self._mode.setText(text or "—")
        self._mode.setStyleSheet(f"background: {color}; color: white; font-weight: bold; "
                                 f"{MONO_CSS} padding: 2px 10px; border-radius: 3px;")

    def _paint_strip(self, silent: bool) -> None:
        bg = SILENCE_BG if silent else STRIP_BG
        self.setStyleSheet(f"QWidget#topStrip {{ background: {bg}; border-bottom: 1px solid #2a2a2a; }}")

    # -- inputs -----------------------------------------------------------------
    def on_packet_received(self, session_id: str, rx_mono: float) -> None:
        """Called for every accepted frame (before `set_state`)."""
        self._last_rx_mono = rx_mono
        self._rx_times.append(rx_mono)
        if session_id != self._session:
            self._session = session_id
            self._t0_mono = rx_mono
            epoch = session_epoch(session_id)
            self._t0_wall = float(epoch) if epoch is not None else None
            self._rx_times.clear()
            self._rx_times.append(rx_mono)

    def set_health(self, pkt: TelemetryPacket) -> None:
        color, tooltip = health_summary(pkt)
        self._health_dot.set_color(color)
        self._health_dot.setToolTip(tooltip)
        self._health_box.setToolTip(tooltip)

    def health_color(self) -> str:
        return self._health_dot.color()

    def health_tooltip(self) -> str:
        return self._health_dot.toolTip()

    def set_state(self, state: OnboardState) -> None:
        if state.have_packet:
            self._paint_mode(state.mode, mode_color(state.mode))
            self._phase.setText(state.phase or "—")
            self._phase.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {phase_color(state.phase)}; border: none;")
            self._sess_full = f"{state.session_id} · seq {state.seq}"
        self.set_silence(state.silence)

    def set_silence(self, active: bool) -> None:
        if active and self._silence_since is None:
            self._silence_since = time.monotonic()
        elif not active:
            self._silence_since = None
        self._paint_strip(active)
        self._radio_dot.set_color(AMBER if active else GREEN)
        self._radio.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {AMBER if active else GREEN}; border: none;")
        self.btn_heaters_off.set_reason("radio silence active" if active else None)
        self.btn_stop_motors.set_reason("radio silence active" if active else None)
        self.btn_enter_safe.set_reason("radio silence active" if active else None)
        self.refresh()

    def set_replay(self, behind_s: Optional[float], eta_s: Optional[float] = None, *,
                   live_panels: bool = False, backlog_frames: Optional[int] = None) -> None:
        """Show the backlog replay (None = nothing replaying). With
        `live_panels` the panels are current and only the queue depth
        matters; without it the arriving frames are `behind_s` old."""
        if behind_s is None:
            self._replay_box.hide()
            return
        eta = f" · ETA {format_elapsed(eta_s)}" if eta_s else ""
        if live_panels:
            text = (f"{backlog_frames} queued" if backlog_frames is not None else "draining") + eta
            tip = "The onboard is replaying its queued backlog into the plots and logs; the panels are live."
        else:
            text = f"−{format_elapsed(behind_s)}" + eta
            tip = "The onboard is replaying its queued backlog; the panels keep showing the last live frame."
        self._replay.setText(text)
        self._replay.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {AMBER}; border: none;")
        self._replay_box.setToolTip(tip)
        self._replay_box.show()

    def replay_visible(self) -> bool:
        return not self._replay_box.isHidden()

    def set_target(self, text: str, color: str = GREEN) -> None:
        self._target_full = text
        self._target.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {color}; border: none;")
        self._elide()

    def _elide(self) -> None:
        for label, full in ((self._target, self._target_full), (self._sess, self._sess_full)):
            metrics = QFontMetrics(label.font())
            width = max(20, label.width() - 4)
            label.setText(metrics.elidedText(full, Qt.TextElideMode.ElideMiddle, width))
            label.setToolTip(full if label.text() != full else "")

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        self._elide()

    def set_receiver_state(self, state: str) -> None:
        self._receiver_state = state
        self.refresh()

    # -- periodic ---------------------------------------------------------------
    def link_age_s(self) -> Optional[float]:
        if self._last_rx_mono is None:
            return None
        return time.monotonic() - self._last_rx_mono

    def refresh(self) -> None:
        now = time.monotonic()
        self._elide()
        self._utc.setText(datetime.now(timezone.utc).strftime("%H:%M:%S"))
        if self._silence_since is not None:
            self._radio.setText(f"SILENT {format_elapsed(now - self._silence_since)}")
        else:
            self._radio.setText("TX")
        age = self.link_age_s()
        if age is None:
            self._link.setText(self._receiver_state if self._receiver_state else "waiting")
            self._link.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {MUTED}; border: none;")
            self._link_dot.set_color(GRAY)
        else:
            if self._silence_since is not None:
                color, tag = AMBER, "silent"
            elif age < self.STALE_AMBER_S:
                color, tag = GREEN, "OK"
            elif age < self.STALE_RED_S:
                color, tag = AMBER, "stale"
            else:
                color, tag = RED, "STALE"
            self._link.setText(f"{tag} {age:4.1f} s")
            self._link.setStyleSheet(f"{MONO_CSS} font-weight: bold; color: {color}; border: none;")
            self._link_dot.set_color(color)
        while self._rx_times and (now - self._rx_times[0]) > self.RATE_WINDOW_S:
            self._rx_times.popleft()
        if self._last_rx_mono is None:
            self._rx.setText("—")
        else:
            self._rx.setText(f"{len(self._rx_times) / self.RATE_WINDOW_S:.1f} Hz")
        # T+ counts from the onboard session start (its boot epoch, embedded
        # in the session id) so it survives a ground-station restart; a
        # session id without an epoch falls back to the first frame seen.
        if self._t0_wall is not None:
            self._tplus.setText(format_elapsed(time.time() - self._t0_wall))
        elif self._t0_mono is not None:
            self._tplus.setText(format_elapsed(now - self._t0_mono))

    # -- test accessors ---------------------------------------------------------
    def link_text(self) -> str:
        return self._link.text()

    def mode_text(self) -> str:
        return self._mode.text()

    def radio_text(self) -> str:
        return self._radio.text()

    def session_text(self) -> str:
        return self._sess_full


class AlarmStrip(QWidget):
    """Chips for active alarms; hidden while there are none."""

    ack_requested = pyqtSignal(str)
    ack_all_requested = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("alarmStrip")
        self.setStyleSheet("QWidget#alarmStrip { background: #221414; border-bottom: 1px solid #3a1f1f; }")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 3, 8, 3)
        lay.setSpacing(6)
        self._chips_box = QWidget()
        self._chips = QHBoxLayout(self._chips_box)
        self._chips.setContentsMargins(0, 0, 0, 0)
        self._chips.setSpacing(6)
        lay.addWidget(self._chips_box, 1)
        self._count = QLabel("")
        self._count.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(self._count)
        self._ack_all = QPushButton("ACK ALL")
        style_button(self._ack_all, "neutral", min_height=22)
        self._ack_all.clicked.connect(self.ack_all_requested.emit)
        lay.addWidget(self._ack_all)
        self._alarms: List[Alarm] = []
        self.hide()

    def set_alarms(self, alarms: List[Alarm]) -> None:
        self._alarms = list(alarms)
        while self._chips.count():
            item = self._chips.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        for alarm in self._alarms:
            chip = QPushButton(f"⚠ {alarm.text}")
            bg = "#c0392b" if alarm.severity == "red" else "#b9770e"
            chip.setStyleSheet(
                f"QPushButton {{ background: {bg}; color: white; font-weight: bold; font-size: 9pt; "
                f"border: none; border-radius: 3px; padding: 2px 8px; }}"
                + ("QPushButton { color: #ddd; background: #4a2a2a; }" if alarm.acked else ""))
            chip.setToolTip("acknowledged — stays until the condition clears" if alarm.acked
                            else "click to acknowledge")
            chip.clicked.connect(lambda _c=False, key=alarm.key: self.ack_requested.emit(key))
            self._chips.addWidget(chip)
        self._chips.addStretch()
        unacked = sum(1 for a in self._alarms if not a.acked)
        self._count.setText(f"{len(self._alarms)} active · {unacked} unacked")
        self._ack_all.setEnabled(unacked > 0)
        self.setVisible(bool(self._alarms))

    def chip_texts(self) -> List[str]:
        return [f"⚠ {a.text}" for a in self._alarms]
