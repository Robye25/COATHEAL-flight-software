"""System tab: link, mode, phase, radio, downlink rate, diagnostics,
shutdown (redesign spec §5.3)."""
from __future__ import annotations

from typing import Optional

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QComboBox, QDoubleSpinBox, QGridLayout, QHBoxLayout, QLabel, QScrollArea,
    QVBoxLayout, QWidget,
)

from ..protocol import CommandResponse, validate_tick_hz
from . import gating
from .dispatch import CommandDispatcher
from .state import OnboardState
from .theme import mode_color, phase_color
from .widgets import (
    AMBER, GREEN, MONO_CSS, MUTED, RED, Indicator, ResponseLine, confirm,
    group_box, hrow, make_button,
)

PHASES = ["BOOT", "ASCENT", "PRE_FLOAT", "FLOAT", "DESCENT", "LANDED", "STOPPED"]
CHECK_TARGETS = ["ALL", "DPS310", "ADS1115", "SEQUENT_RTD", "MAX31865", "PWM", "MOTOR0", "MOTOR1", "STORAGE", "COMMS"]


class SystemTab(QScrollArea):
    def __init__(self, dispatcher: CommandDispatcher, parent=None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self._disp = dispatcher
        self._state = OnboardState()
        inner = QWidget()
        self.setWidget(inner)
        outer = QVBoxLayout(inner)
        outer.setContentsMargins(6, 6, 6, 6)
        outer.setSpacing(8)

        # -- Link ------------------------------------------------------------
        frame, lay = group_box("Link")
        self.i_target = Indicator("Command target")
        self.i_receiver = Indicator("Telemetry receiver")
        self.i_rate = Indicator("Frames")
        self.i_uplink = Indicator("Onboard heard uplink")
        self.i_uplink.setToolTip("CTRL link_loss_s: how long ago the onboard last heard this ground station")
        self.i_queue = Indicator("Onboard queue")
        self.i_session = Indicator("Session")
        for ind in (self.i_target, self.i_receiver, self.i_rate, self.i_uplink, self.i_queue, self.i_session):
            lay.addWidget(ind)
        self.btn_restart_receiver = make_button("RESTART RECEIVER", "neutral", min_height=24)
        self.btn_restart_receiver.setToolTip("Restarts the local telemetry receiver — does not send a wire command.")
        self.btn_restart_receiver.hide()
        lay.addWidget(self.btn_restart_receiver)
        outer.addWidget(frame)

        # -- Mode ------------------------------------------------------------
        frame, lay = group_box("Mode")
        self._mode_tile = QLabel("—")
        self._mode_tile.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._mode_tile.setMinimumHeight(40)
        self._paint_mode("", "#7f8c8d")
        lay.addWidget(self._mode_tile)
        grid = QGridLayout(); grid.setSpacing(4)
        self.btn_arm = make_button("ARM", "success", sends="ARM", slot=self._arm)
        self.btn_disarm = make_button("DISARM", "danger", sends="DISARM", slot=lambda: self._send("DISARM"))
        self.btn_enter_safe = make_button("ENTER SAFE", "danger", sends="ENTER_SAFE", slot=self._enter_safe)
        self.btn_exit_safe = make_button("EXIT SAFE", "success", sends="EXIT_SAFE", slot=lambda: self._send("EXIT_SAFE"))
        grid.addWidget(self.btn_arm, 0, 0); grid.addWidget(self.btn_disarm, 0, 1)
        grid.addWidget(self.btn_enter_safe, 1, 0); grid.addWidget(self.btn_exit_safe, 1, 1)
        lay.addLayout(grid)
        self.resp_mode = ResponseLine()
        lay.addWidget(self.resp_mode)
        outer.addWidget(frame)

        # -- Phase -----------------------------------------------------------
        frame, lay = group_box("Phase")
        self.i_phase = Indicator("Current phase")
        lay.addWidget(self.i_phase)
        self.phase_select = QComboBox()
        self.phase_select.addItems(PHASES)
        self.btn_set_phase = make_button("SET PHASE", "primary", sends="SET_PHASE <phase>", slot=self._set_phase)
        lay.addWidget(hrow(self.phase_select, self.btn_set_phase))
        self.i_fallback = Indicator("Link-loss fallback", value="unknown")
        lay.addWidget(self.i_fallback)
        self.resp_phase = ResponseLine()
        lay.addWidget(self.resp_phase)
        outer.addWidget(frame)

        # -- Radio -----------------------------------------------------------
        frame, lay = group_box("Radio")
        self.i_radio = Indicator("Downlink", value="transmitting")
        lay.addWidget(self.i_radio)
        self.btn_silence = make_button("RADIO SILENCE", "danger", sends="RADIO_SILENCE", min_height=30,
                                       slot=self._radio_silence)
        self.btn_resume = make_button("RADIO RESUME", "success", sends="RADIO_RESUME", min_height=30,
                                      slot=lambda: self._send("RADIO_RESUME"))
        lay.addWidget(hrow(self.btn_silence, self.btn_resume))
        note = QLabel("Silence stops telemetry, beacons and hello replies onboard; the ground station "
                      "pauses its beacon and probe and sends nothing but RADIO RESUME (STATUS/PING answer). "
                      "The onboard queue keeps every frame and replays it after RESUME.")
        note.setWordWrap(True)
        note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.resp_radio = ResponseLine()
        lay.addWidget(self.resp_radio)
        outer.addWidget(frame)

        # -- Downlink rate ---------------------------------------------------
        frame, lay = group_box("Downlink rate")
        self.tick_hz = QDoubleSpinBox()
        self.tick_hz.setRange(0.1, 5.0); self.tick_hz.setDecimals(1); self.tick_hz.setSingleStep(0.5)
        self.tick_hz.setValue(1.0); self.tick_hz.setSuffix(" Hz")
        self.btn_tick = make_button("SET", "primary", sends="SET_TICK_HZ <hz>", slot=self._set_tick, min_height=24)
        lbl = QLabel("0.1–5.0 Hz (BEXUS §5.4)"); lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(self.tick_hz, self.btn_tick, lbl, stretch_last=True))
        self.resp_rate = ResponseLine()
        lay.addWidget(self.resp_rate)
        outer.addWidget(frame)

        # -- Diagnostics -----------------------------------------------------
        frame, lay = group_box("Diagnostics")
        grid = QGridLayout(); grid.setSpacing(4)
        self.btn_ping = make_button("PING", "neutral", sends="PING", slot=lambda: self._send("PING"))
        self.btn_status = make_button("STATUS", "neutral", sends="STATUS", slot=lambda: self._send("STATUS"))
        self.btn_components = make_button("COMPONENTS", "neutral", sends="COMPONENTS", slot=lambda: self._send("COMPONENTS"))
        self.btn_thermal = make_button("GET_THERMAL", "neutral", sends="GET_THERMAL", slot=lambda: self._send("GET_THERMAL"))
        grid.addWidget(self.btn_ping, 0, 0); grid.addWidget(self.btn_status, 0, 1)
        grid.addWidget(self.btn_components, 1, 0); grid.addWidget(self.btn_thermal, 1, 1)
        lay.addLayout(grid)
        self.check_target = QComboBox()
        self.check_target.addItems(CHECK_TARGETS)
        self.btn_check = make_button("CHECK", "neutral", sends="CHECK <component>", slot=self._check)
        self.btn_reset = make_button("RESET_CTRL", "danger", sends="RESET_CTRL", slot=self._reset_ctrl)
        lay.addWidget(hrow(self.check_target, self.btn_check, self.btn_reset))
        note = QLabel("CHECK drives real hardware conversations (up to 15 s); a motor check fails while that motor is moving "
                      "or holding (CHECK ALL needs both idle). RESET_CTRL clears the over-temperature latch and PID integrators.")
        note.setWordWrap(True); note.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(note)
        self.resp_diag = ResponseLine()
        lay.addWidget(self.resp_diag)
        outer.addWidget(frame)

        # -- Shutdown --------------------------------------------------------
        frame, lay = group_box("Shutdown")
        self.btn_shutdown = make_button("SHUTDOWN SAFE", "danger", sends="SHUTDOWN_SAFE", slot=self._shutdown)
        lbl = QLabel("bench / post-landing only — flushes logs and stops the onboard process")
        lbl.setWordWrap(True); lbl.setStyleSheet(f"color: {MUTED}; font-size: 8pt;")
        lay.addWidget(hrow(self.btn_shutdown, lbl, stretch_last=True))
        self.resp_shutdown = ResponseLine()
        lay.addWidget(self.resp_shutdown)
        outer.addWidget(frame)
        outer.addStretch()

        self._response_lines = {
            "ARM": self.resp_mode, "DISARM": self.resp_mode, "ENTER_SAFE": self.resp_mode, "EXIT_SAFE": self.resp_mode,
            "SET_PHASE": self.resp_phase, "RADIO_SILENCE": self.resp_radio, "RADIO_RESUME": self.resp_radio,
            "SET_TICK_HZ": self.resp_rate, "PING": self.resp_diag, "STATUS": self.resp_diag,
            "COMPONENTS": self.resp_diag, "GET_THERMAL": self.resp_diag, "CHECK": self.resp_diag,
            "RESET_CTRL": self.resp_diag, "SHUTDOWN_SAFE": self.resp_shutdown,
        }
        self.update_state(self._state)

    # -- senders -----------------------------------------------------------------
    def _send(self, cmd: str) -> None:
        self._disp.send(cmd, tag=self)

    def _arm(self) -> None:
        if confirm(self, "Arm the experiment?", "Send ARM? Heater and motor commands become live."):
            self._send("ARM")

    def _enter_safe(self) -> None:
        if confirm(self, "Enter SAFE mode?", "Send ENTER_SAFE? Heaters off, motors stopped, logs synced."):
            self._send("ENTER_SAFE")

    def _set_phase(self) -> None:
        phase = self.phase_select.currentText()
        if confirm(self, "Set mission phase?", f"Send SET_PHASE {phase}?"):
            self._send(f"SET_PHASE {phase}")

    def _radio_silence(self) -> None:
        if confirm(self, "Radio silence?", "Send RADIO_SILENCE? All onboard transmission stops until RADIO_RESUME."):
            self._send("RADIO_SILENCE")

    def _set_tick(self) -> None:
        ok, norm = validate_tick_hz(self.tick_hz.value())
        if not ok:
            self.resp_rate.show_note(f"✖ {norm}", RED)
            return
        self._send(f"SET_TICK_HZ {norm}")

    def _check(self) -> None:
        target = self.check_target.currentText()
        self._send("CHECK" if target == "ALL" else f"CHECK {target}")

    def _reset_ctrl(self) -> None:
        if confirm(self, "Reset controller?", "Send RESET_CTRL? Clears the over-temperature latch and PID integrators."):
            self._send("RESET_CTRL")

    def _shutdown(self) -> None:
        if confirm(self, "Shut down the onboard?", "Send SHUTDOWN_SAFE? Heaters off, logs flushed, onboard process stops."):
            self._send("SHUTDOWN_SAFE")

    # -- inputs ------------------------------------------------------------------
    def _paint_mode(self, text: str, color: str) -> None:
        self._mode_tile.setText(text or "—")
        self._mode_tile.setStyleSheet(f"background: {color}; color: white; font-size: 15pt; font-weight: bold; "
                                      f"{MONO_CSS} border-radius: 4px;")

    def update_state(self, state: OnboardState) -> None:
        self._state = state
        if state.have_packet:
            self._paint_mode(state.mode, mode_color(state.mode))
            self.i_phase.set_value(state.phase, phase_color(state.phase))
            self.i_phase.set_color(phase_color(state.phase))
            if state.phase in PHASES and not self.phase_select.hasFocus():
                self.phase_select.setCurrentText(state.phase)
            if state.fallback is None:
                self.i_fallback.set_value("not reported", MUTED); self.i_fallback.set_color("#666666")
            elif state.fallback:
                self.i_fallback.set_value(f"ACTIVE {state.link_loss_s or 0:.0f} s", RED); self.i_fallback.set_color(RED)
            else:
                self.i_fallback.set_value("inactive", GREEN); self.i_fallback.set_color(GREEN)
            self.i_session.set_value(state.session_id)
            if state.link_loss_s is None:
                self.i_uplink.set_value("not reported", MUTED); self.i_uplink.set_color("#666666")
            else:
                up_color = GREEN if state.link_loss_s < 5.0 else AMBER
                self.i_uplink.set_value(f"{state.link_loss_s:.0f} s ago", up_color)
                self.i_uplink.set_color(up_color)
            if state.queue_depth is None:
                self.i_queue.set_value("not reported", MUTED); self.i_queue.set_color("#666666")
            else:
                self.i_queue.set_value(f"{state.queue_depth} frames", GREEN if state.queue_depth < 100 else AMBER)
                self.i_queue.set_color(GREEN if state.queue_depth < 100 else AMBER)
        self.i_radio.set_value("SILENT" if state.silence else "transmitting", AMBER if state.silence else GREEN)
        self.i_radio.set_color(AMBER if state.silence else GREEN)
        self.btn_arm.set_reason(gating.arm_reason(state))
        self.btn_disarm.set_reason(gating.disarm_reason(state))
        self.btn_exit_safe.set_reason(gating.exit_safe_reason(state))
        self.btn_enter_safe.set_reason(gating.generic_reason(state))
        self.btn_set_phase.set_reason(gating.generic_reason(state))
        self.btn_tick.set_reason(gating.generic_reason(state))
        for btn in (self.btn_components, self.btn_thermal, self.btn_check, self.btn_reset, self.btn_shutdown):
            btn.set_reason(gating.generic_reason(state))
        self.btn_silence.set_reason("already silent" if state.silence else None)
        self.btn_resume.set_reason(None if state.silence else "not silent")

    def set_link_info(self, *, target: str, target_how: str, receiver: str, rate_hz: Optional[float],
                      age_s: Optional[float]) -> None:
        self.i_target.set_value(f"{target} ({target_how})" if target_how else target, GREEN if target else MUTED)
        self.i_target.set_color(GREEN if target else "#666666")
        colors = {"listening": "#3498db", "connected": GREEN, "stale": AMBER, "searching": AMBER, "failed": RED}
        self.i_receiver.set_value(receiver, colors.get(receiver, MUTED))
        self.i_receiver.set_color(colors.get(receiver, "#666666"))
        self.btn_restart_receiver.setVisible(receiver == "failed")
        if rate_hz is None:
            self.i_rate.set_value("—")
        else:
            age = f" · age {age_s:.1f} s" if age_s is not None else ""
            self.i_rate.set_value(f"{rate_hz:.1f} Hz{age}")

    def on_response(self, cmd: str, resp: CommandResponse, ms: float, tag) -> None:
        if tag is not self:
            return
        verb = cmd.strip().split()[0].upper() if cmd.strip() else ""
        line = self._response_lines.get(verb)
        if line is not None:
            line.show_response(cmd, resp, ms)
