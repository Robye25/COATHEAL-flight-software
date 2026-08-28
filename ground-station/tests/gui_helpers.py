"""Shared helpers for the headless GUI tests: build a real MainWindow on
free ports, feed scripted frames, capture dispatcher sends."""
from __future__ import annotations

import os
import socket
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def pump_until(app, predicate, timeout_s: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


def make_window(tmp_root: Path, *, cmd_host: str = "127.0.0.1"):
    from app.gui.main_window import MainWindow
    from app.thermal_presets import PresetStore
    return MainWindow(
        bind="127.0.0.1", tel_port=free_port(), cmd_port=free_port(), cmd_host=cmd_host,
        log_path=tmp_root, firewall_check=False,
        preset_store=PresetStore(tmp_root / "profiles" / "thermal_presets.json").load(),
    )


def capture_sends(dispatcher) -> list:
    sent: list = []
    dispatcher.send = lambda cmd, tag=None, timeout=None: sent.append(cmd)
    return sent


def frame(*, seq: int = 1, session: str = "coatheal-1787760547-1", mode: str = "RUN", phase: str = "ASCENT",
          status: str = "SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK"
                        "|ENERGY_OK|PWM_OK|STEPPER_OK|SAMPLE_TEMP_OK|REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK",
          samples: str = "1,2,3,4,5,6,7,8",
          valid: str = "AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1",
          comps: str = "DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK",
          ctrl: str = "fallback:0|link_loss_s:0.0|energy_wh:1.0|budget_wh:130.0|budget_exhausted:0|heaters_active:0|queue:0|plan:none",
          m0: str = "en:1|ok:1|mv:0|hold:0|zeroed:1", m1: str = "en:0|ok:1|mv:0|hold:0|zeroed:0",
          resistance: str = "118|-|-|-|121|-|-|-", duties: str = "0|0|0|0|0|0") -> str:
    return (f"DATA,{session},{seq},2026-08-28T00:00:00Z,1,20,1000,0.1,{samples},"
            f"HEATER_DUTY={duties},RESISTANCE={resistance},PHASE={phase},MODE={mode},STATUS={status},"
            f"SENSOR_VALID={valid},SENSOR_AGE_MS=AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1,"
            f"COMPONENT_STATE={comps},CTRL={ctrl},"
            f"STEPPER0=pos:0|tgt:0|hz:100|us:4|hold_s:0|pulses:0|src:init|seq:-|seqst:idle|{m0},"
            f"STEPPER1=pos:0|tgt:0|hz:100|us:4|hold_s:0|pulses:0|src:init|seq:-|seqst:idle|{m1}")
