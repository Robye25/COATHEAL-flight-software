#!/usr/bin/env python3
"""COATHEAL Ground Station — PyQt6 + PyQtGraph real-time GUI

Usage:
    cd ground-station
    python gui_app.py [--tel-port 4000] [--cmd-port 5000] [--host 169.254.10.10]
"""
from __future__ import annotations

import argparse
import csv
import json
import socket
import sys
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import Qt, QThread, QTimer, pyqtSignal, pyqtSlot
from PyQt6.QtGui import QAction, QColor, QFont, QPalette
from PyQt6.QtWidgets import (
    QApplication, QDialog, QDialogButtonBox, QDockWidget, QDoubleSpinBox,
    QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMainWindow,
    QMessageBox, QProgressBar, QPushButton, QScrollArea, QSizePolicy,
    QSpinBox, QSplitter, QStatusBar, QTabWidget, QTextEdit, QVBoxLayout,
    QWidget, QFrame, QGridLayout, QCheckBox, QFileDialog,
)

sys.path.insert(0, str(Path(__file__).parent))
from app.protocol import (
    TelemetryPacket, TelemetryParseError, build_ack, parse_telemetry_csv,
)

# ── globals ────────────────────────────────────────────────────────
MAX_POINTS = 1200   # rolling window depth (~20 min at 1 Hz)

HEATER_COLORS = [
    "#e74c3c", "#e67e22", "#f1c40f", "#2ecc71", "#1abc9c",
    "#3498db", "#9b59b6", "#e91e63", "#00bcd4", "#ff6b35",
]
HEATER_LABELS = [f"H{i}" for i in range(9)] + ["BOX"]

PHASE_COLORS = {
    "ASCENT_HOLD":       "#3498db",
    "ACTIVATION_RAMP":   "#f39c12",
    "FLOAT_HOLD":        "#2ecc71",
    "DESCENT_FLOOR":     "#9b59b6",
    "STOPPED":           "#e74c3c",
}


def phase_color(phase: str) -> str:
    for key, color in PHASE_COLORS.items():
        if key in phase.upper():
            return color
    return "#aaaaaa"


# ── telemetry receiver (background QThread) ────────────────────────
class TelemetryReceiver(QThread):
    packet_received    = pyqtSignal(object)        # TelemetryPacket
    log_message        = pyqtSignal(str)
    connection_changed = pyqtSignal(bool, str)      # connected, addr_str

    def __init__(self, bind: str, port: int, log_path: Path, parent=None):
        super().__init__(parent)
        self._bind = bind
        self._port = port
        self._log_path = log_path
        self._stop_flag = threading.Event()
        self._last_seq_by_session: dict[str, int] = {}
        self._load_cursor()

    # ── cursor (de-dup / resume) ───────────────────────────────────
    def _cursor_path(self) -> Path:
        return self._log_path.parent / "ground_ack_cursor.json"

    def _load_cursor(self) -> None:
        p = self._cursor_path()
        if not p.exists():
            return
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            self._last_seq_by_session = {
                str(k): int(v) for k, v in data.get("sessions", {}).items()
            }
        except Exception:
            pass

    def _persist_cursor(self) -> None:
        payload = {
            "updated_utc": datetime.now(timezone.utc).isoformat(),
            "sessions": self._last_seq_by_session,
        }
        try:
            self._cursor_path().write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except OSError:
            pass

    def stop(self) -> None:
        self._stop_flag.set()

    # ── thread entry point ─────────────────────────────────────────
    def run(self) -> None:
        self._log_path.parent.mkdir(parents=True, exist_ok=True)
        first_write = not self._log_path.exists()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind((self._bind, self._port))
                srv.listen(5)  # backlog=5 so Pi retry attempts queue up during reconnect
                srv.settimeout(1.0)
                self.log_message.emit(
                    f"[telemetry] listening on {self._bind}:{self._port}"
                )
                while not self._stop_flag.is_set():
                    try:
                        conn, addr = srv.accept()
                    except socket.timeout:
                        continue
                    addr_str = f"{addr[0]}:{addr[1]}"
                    self.connection_changed.emit(True, addr_str)
                    self.log_message.emit(
                        f"[telemetry] onboard connected from {addr_str}"
                    )
                    try:
                        self._handle_connection(conn, first_write)
                        first_write = False
                    except OSError as exc:
                        # Connection reset by remote (e.g. WinError 10054) — treat as
                        # a normal disconnect and loop back to accept().
                        self.log_message.emit(f"[telemetry] connection reset: {exc}")
                    finally:
                        conn.close()
                        self.connection_changed.emit(False, "")
                        self.log_message.emit("[telemetry] onboard disconnected")
        except Exception as exc:
            # Only truly fatal errors (bind failure, etc.) reach here.
            self.log_message.emit(f"[error] receiver fatal: {exc}")
        finally:
            self._stop_flag.set()

    # Must be shorter than the Pi's reconnect_ms (2000 ms) × retry cycles so we
    # get back to srv.accept() before the Pi exhausts its connection attempts.
    # 1 Hz telemetry → 3 missed packets before declaring the connection dead.
    _DATA_TIMEOUT_S = 3.0

    def _handle_connection(self, conn: socket.socket, write_header: bool) -> None:
        conn.settimeout(1.0)
        # TCP keepalive so the OS also detects dead connections independently
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        buf = ""
        last_data = time.monotonic()
        with self._log_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "session_id", "seq", "timestamp", "rtc_valid",
                "ambient_temp_c", "ambient_pressure_mbar", "ambient_humidity_pct",
                "uv", "box_temp_c", "sample_temps_c", "heater_duty", "phase", "status",
            ])
            if write_header:
                writer.writeheader()

            while not self._stop_flag.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    if time.monotonic() - last_data > self._DATA_TIMEOUT_S:
                        self.log_message.emit(
                            f"[telemetry] no data for {self._DATA_TIMEOUT_S:.0f}s "
                            "— closing stale connection, waiting for reconnect"
                        )
                        return
                    continue
                except OSError:
                    # Connection reset by remote host (e.g. WinError 10054).
                    return
                if not chunk:
                    break
                last_data = time.monotonic()

                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        pkt = parse_telemetry_csv(line)
                    except TelemetryParseError as exc:
                        self.log_message.emit(f"[parse-error] {exc}")
                        continue

                    last_seq = self._last_seq_by_session.get(pkt.session_id, -1)
                    is_dup = pkt.seq <= last_seq
                    if not is_dup:
                        self._last_seq_by_session[pkt.session_id] = pkt.seq
                        self._persist_cursor()

                    try:
                        conn.sendall(build_ack(pkt.session_id, pkt.seq).encode("utf-8"))
                    except OSError:
                        return

                    if is_dup:
                        self.log_message.emit(
                            f"[dup] dropped seq={pkt.seq} session={pkt.session_id}"
                        )
                        continue

                    row = {k: getattr(pkt, k) for k in [
                        "session_id", "seq", "timestamp", "rtc_valid",
                        "ambient_temp_c", "ambient_pressure_mbar",
                        "ambient_humidity_pct", "uv", "box_temp_c", "phase", "status",
                    ]}
                    row["sample_temps_c"] = "|".join(f"{x:.2f}" for x in pkt.sample_temps_c)
                    row["heater_duty"]    = "|".join(f"{x:.3f}" for x in pkt.heater_duty)
                    writer.writerow(row)
                    f.flush()

                    self.packet_received.emit(pkt)


# ── command sender (synchronous, called from main thread) ──────────
class CommandSender:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port

    def send(self, cmd: str) -> tuple[bool, str]:
        """Returns (success, response_or_error)."""
        try:
            payload = cmd.strip() + "\n"
            with socket.create_connection((self.host, self.port), timeout=3.0) as s:
                s.sendall(payload.encode("utf-8"))
                resp = s.recv(4096).decode("utf-8", errors="replace").strip()
            return True, resp
        except Exception as exc:
            return False, str(exc)


# ── real-time multi-trace plot widget ──────────────────────────────
class LivePlotWidget(QWidget):
    def __init__(self, title: str, y_label: str, unit: str = "", parent=None):
        super().__init__(parent)

        self._plot = pg.PlotWidget()
        self._plot.setTitle(title, color="#dddddd", size="11pt")
        self._plot.setLabel("left", y_label, units=unit)
        self._plot.setLabel("bottom", "Sequence")
        self._plot.addLegend(offset=(10, 10))
        self._plot.showGrid(x=True, y=True, alpha=0.25)
        self._plot.setMenuEnabled(True)     # right-click menu: export, zoom, etc.
        self._plot.setClipToView(True)

        self._curves: dict[str, pg.PlotDataItem] = {}
        self._x: deque[int]             = deque(maxlen=MAX_POINTS)
        self._y: dict[str, deque[float]] = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._plot)

    def add_curve(self, name: str, color: str, width: int = 2) -> None:
        if name in self._curves:
            return
        pen = pg.mkPen(color=color, width=width)
        self._curves[name] = self._plot.plot([], [], pen=pen, name=name)
        self._y[name] = deque(maxlen=MAX_POINTS)

    def push(self, seq: int, values: dict[str, float]) -> None:
        self._x.append(seq)
        x = np.array(self._x)
        for name, val in values.items():
            if name not in self._y:
                self._y[name] = deque(maxlen=MAX_POINTS)
            self._y[name].append(val)
            if name in self._curves:
                y = np.array(self._y[name])
                n = min(len(x), len(y))
                self._curves[name].setData(x[-n:], y[-n:])

    def clear(self) -> None:
        self._x.clear()
        for d in self._y.values():
            d.clear()
        for c in self._curves.values():
            c.setData([], [])


# ── heater duty bars panel ─────────────────────────────────────────
class HeaterPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        grid = QGridLayout(self)
        grid.setSpacing(3)
        grid.setContentsMargins(4, 4, 4, 4)

        self._bars:   list[QProgressBar] = []
        self._pct_lbls: list[QLabel]     = []

        for i in range(10):
            name_lbl = QLabel(HEATER_LABELS[i])
            name_lbl.setFixedWidth(32)
            name_lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

            bar = QProgressBar()
            bar.setRange(0, 1000)
            bar.setValue(0)
            bar.setTextVisible(False)
            bar.setFixedHeight(16)
            color = HEATER_COLORS[i]
            bar.setStyleSheet(
                f"QProgressBar {{ border: 1px solid #333; border-radius: 3px; background: #111; }}"
                f"QProgressBar::chunk {{ background-color: {color}; border-radius: 2px; }}"
            )

            pct_lbl = QLabel("  0.0%")
            pct_lbl.setFixedWidth(46)
            pct_lbl.setStyleSheet("font-family: monospace; font-size: 10px;")

            grid.addWidget(name_lbl, i, 0)
            grid.addWidget(bar,      i, 1)
            grid.addWidget(pct_lbl,  i, 2)
            self._bars.append(bar)
            self._pct_lbls.append(pct_lbl)

    def update_duties(self, duties: list[float]) -> None:
        for i, d in enumerate(duties[:10]):
            self._bars[i].setValue(int(d * 1000))
            self._pct_lbls[i].setText(f"{d * 100:5.1f}%")


# ── telemetry values panel ─────────────────────────────────────────
class ValuesPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

        self._fields: dict[str, QLabel] = {}

        def section(title: str) -> None:
            lbl = QLabel(title)
            lbl.setStyleSheet(
                "font-weight: bold; color: #aaa; font-size: 10px; "
                "border-bottom: 1px solid #333; margin-top: 6px;"
            )
            layout.addWidget(lbl)

        def row(key: str, label: str) -> None:
            w = QWidget()
            h = QHBoxLayout(w)
            h.setContentsMargins(0, 0, 0, 0)
            name = QLabel(label)
            name.setMinimumWidth(80)
            name.setStyleSheet("color: #888; font-size: 11px;")
            val = QLabel("—")
            val.setStyleSheet("font-family: monospace; font-size: 11px;")
            h.addWidget(name)
            h.addWidget(val)
            h.addStretch()
            layout.addWidget(w)
            self._fields[key] = val

        section("SESSION")
        row("phase",     "Phase")
        row("seq",       "Sequence")
        row("timestamp", "Timestamp")
        row("rtc",       "RTC Valid")
        row("session",   "Session ID")

        section("ENVIRONMENT")
        row("pressure",  "Pressure")
        row("amb_temp",  "Amb. Temp")
        row("humidity",  "Humidity")
        row("uv",        "UV")

        section("THERMAL")
        row("box_temp",  "Box Temp")
        for i in range(9):
            row(f"s{i}", f"Sample {i}")

        section("STATUS FLAGS")
        for flag in ("SD", "USB", "I2C", "SPI", "LINK"):
            row(flag.lower(), flag)

        layout.addStretch()

    def update_packet(self, pkt: TelemetryPacket) -> None:
        self._fields["phase"].setText(pkt.phase)
        self._fields["phase"].setStyleSheet(
            f"font-family: monospace; font-size: 11px; color: {phase_color(pkt.phase)}; font-weight: bold;"
        )
        self._fields["seq"].setText(str(pkt.seq))
        self._fields["timestamp"].setText(pkt.timestamp)
        self._fields["rtc"].setText("YES" if pkt.rtc_valid else "NO")
        self._fields["session"].setText(pkt.session_id[:12] + "…" if len(pkt.session_id) > 12 else pkt.session_id)
        self._fields["pressure"].setText(f"{pkt.ambient_pressure_mbar:.2f} mbar")
        self._fields["amb_temp"].setText(f"{pkt.ambient_temp_c:.2f} °C")
        self._fields["humidity"].setText(f"{pkt.ambient_humidity_pct:.1f} %")
        self._fields["uv"].setText(f"{pkt.uv:.5f}")
        self._fields["box_temp"].setText(f"{pkt.box_temp_c:.2f} °C")
        for i, t in enumerate(pkt.sample_temps_c[:9]):
            self._fields[f"s{i}"].setText(f"{t:.2f} °C")

        flags_raw = pkt.status.split("|")
        flag_map = {}
        for f in flags_raw:
            parts = f.split("_", 1)
            if len(parts) == 2:
                flag_map[parts[0]] = parts[1] == "OK"

        for flag in ("SD", "USB", "I2C", "SPI", "LINK"):
            ok = flag_map.get(flag, None)
            if ok is None:
                text, color = "?", "#888"
            elif ok:
                text, color = "OK", "#2ecc71"
            else:
                text, color = "FAIL", "#e74c3c"
            self._fields[flag.lower()].setText(text)
            self._fields[flag.lower()].setStyleSheet(
                f"font-family: monospace; font-size: 11px; color: {color}; font-weight: bold;"
            )


# ── command panel ──────────────────────────────────────────────────
DANGEROUS_CMDS = {"FORCE_STOP", "HEATERS_OFF", "RESET_CTRL", "SHUTDOWN_SAFE"}


class CommandPanel(QWidget):
    log_message = pyqtSignal(str)

    def __init__(self, sender: CommandSender, parent=None):
        super().__init__(parent)
        self._sender = sender
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(5)

        def section_label(text: str) -> QLabel:
            lbl = QLabel(text)
            lbl.setStyleSheet(
                "color: #888; font-size: 10px; border-top: 1px solid #333; margin-top: 4px; padding-top: 4px;"
            )
            return lbl

        def btn(label: str, cmd: str, color: str, dangerous: bool = False) -> QPushButton:
            b = QPushButton(label)
            b.setStyleSheet(
                f"background-color: {color}; color: white; font-weight: bold; "
                f"padding: 5px 8px; border-radius: 3px;"
            )
            b.clicked.connect(lambda: self._simple(cmd, dangerous))
            return b

        # ── safe commands ──────────────────────────────────────────
        layout.addWidget(section_label("SAFE"))
        row1 = QHBoxLayout()
        row1.addWidget(btn("PING",   "PING",   "#2980b9"))
        row1.addWidget(btn("STATUS", "STATUS", "#2980b9"))
        layout.addLayout(row1)

        # ── phase control ──────────────────────────────────────────
        layout.addWidget(section_label("PHASE CONTROL"))
        layout.addWidget(btn("FORCE START", "FORCE_START", "#d35400"))
        row2 = QHBoxLayout()
        row2.addWidget(btn("FORCE STOP",  "FORCE_STOP",  "#c0392b", dangerous=True))
        row2.addWidget(btn("RESET CTRL",  "RESET_CTRL",  "#8e44ad", dangerous=True))
        layout.addLayout(row2)

        # ── emergency ─────────────────────────────────────────────
        layout.addWidget(section_label("EMERGENCY"))
        hoff = btn("⚠ HEATERS OFF", "HEATERS_OFF", "#c0392b", dangerous=True)
        hoff.setFixedHeight(34)
        layout.addWidget(hoff)
        shut = btn("⚠ SHUTDOWN SAFE", "SHUTDOWN_SAFE", "#922b21", dangerous=True)
        shut.setFixedHeight(34)
        layout.addWidget(shut)

        # ── debug arm ─────────────────────────────────────────────
        layout.addWidget(section_label("DEBUG"))
        arm_row = QHBoxLayout()
        self._arm_token = QLineEdit()
        self._arm_token.setPlaceholderText("debug token")
        arm_btn = QPushButton("ARM DEBUG")
        arm_btn.setStyleSheet(
            "background-color: #6c3483; color: white; font-weight: bold; "
            "padding: 5px; border-radius: 3px;"
        )
        arm_btn.clicked.connect(self._send_arm)
        arm_row.addWidget(self._arm_token)
        arm_row.addWidget(arm_btn)
        layout.addLayout(arm_row)

        disarm = QPushButton("DISARM DEBUG")
        disarm.setStyleSheet(
            "background-color: #4a235a; color: white; padding: 5px; border-radius: 3px;"
        )
        disarm.clicked.connect(lambda: self._send_cmd("DISARM_DEBUG"))
        layout.addWidget(disarm)

        # ── bench mode toggle ──────────────────────────────────────
        bench_row = QHBoxLayout()
        bench_on  = QPushButton("BENCH ON")
        bench_off = QPushButton("BENCH OFF")
        bench_on.setStyleSheet(
            "background-color: #1a5276; color: white; padding: 5px; border-radius: 3px;"
        )
        bench_off.setStyleSheet(
            "background-color: #1a5276; color: white; padding: 5px; border-radius: 3px;"
        )
        bench_on.clicked.connect(lambda: self._send_cmd("SET_BENCH_MODE 1"))
        bench_off.clicked.connect(lambda: self._send_cmd("SET_BENCH_MODE 0"))
        bench_row.addWidget(bench_on)
        bench_row.addWidget(bench_off)
        layout.addLayout(bench_row)

        # ── heater overrides ──────────────────────────────────────
        layout.addWidget(section_label("HEATER OVERRIDES (requires ARM DEBUG)"))

        hd_form = QFormLayout()
        hd_form.setSpacing(4)
        self._hd_idx  = QSpinBox()
        self._hd_idx.setRange(0, 9)
        self._hd_duty = QDoubleSpinBox()
        self._hd_duty.setRange(0.0, 1.0)
        self._hd_duty.setSingleStep(0.05)
        self._hd_duty.setDecimals(2)
        hd_form.addRow("Heater index:", self._hd_idx)
        hd_form.addRow("Duty [0–1]:",   self._hd_duty)
        layout.addLayout(hd_form)

        hd_btn = QPushButton("SET HEATER DUTY")
        hd_btn.setStyleSheet(
            "background-color: #148f77; color: white; font-weight: bold; "
            "padding: 5px; border-radius: 3px;"
        )
        hd_btn.clicked.connect(self._send_set_heater)
        layout.addWidget(hd_btn)

        all_form = QFormLayout()
        all_form.setSpacing(4)
        self._all_duty = QDoubleSpinBox()
        self._all_duty.setRange(0.0, 1.0)
        self._all_duty.setSingleStep(0.05)
        self._all_duty.setDecimals(2)
        all_form.addRow("All duty [0–1]:", self._all_duty)
        layout.addLayout(all_form)

        all_btn = QPushButton("SET ALL DUTY")
        all_btn.setStyleSheet(
            "background-color: #148f77; color: white; font-weight: bold; "
            "padding: 5px; border-radius: 3px;"
        )
        all_btn.clicked.connect(self._send_set_all)
        layout.addWidget(all_btn)

        clear_btn = QPushButton("CLEAR OVERRIDES")
        clear_btn.setStyleSheet(
            "background-color: #1e8449; color: white; padding: 5px; border-radius: 3px;"
        )
        clear_btn.clicked.connect(lambda: self._send_cmd("CLEAR_OVERRIDES"))
        layout.addWidget(clear_btn)

        layout.addStretch()

    def set_sender(self, sender: CommandSender) -> None:
        self._sender = sender

    def _send_cmd(self, cmd: str) -> None:
        ok, resp = self._sender.send(cmd)
        if ok:
            self.log_message.emit(f"[cmd] ← {resp}")
        else:
            self.log_message.emit(f"[cmd-error] {cmd}: {resp}")

    def _simple(self, cmd: str, dangerous: bool) -> None:
        if dangerous:
            box = QMessageBox(self)
            box.setWindowTitle("Confirm safety-critical command")
            box.setText(f"Send <b>{cmd}</b>?")
            box.setStandardButtons(
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel
            )
            box.setDefaultButton(QMessageBox.StandardButton.Cancel)
            if box.exec() != QMessageBox.StandardButton.Yes:
                return
        self._send_cmd(cmd)

    def _send_arm(self) -> None:
        token = self._arm_token.text().strip() or "debug"
        self._send_cmd(f"ARM_DEBUG {token}")

    def _send_set_heater(self) -> None:
        self._send_cmd(f"SET_HEATER_DUTY {self._hd_idx.value()} {self._hd_duty.value():.2f}")

    def _send_set_all(self) -> None:
        self._send_cmd(f"SET_ALL_DUTY {self._all_duty.value():.2f}")


# ── log viewer panel ───────────────────────────────────────────────
class LogPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        toolbar = QHBoxLayout()
        clear_btn = QPushButton("Clear")
        clear_btn.setFixedWidth(60)
        clear_btn.clicked.connect(self._clear)
        save_btn = QPushButton("Save…")
        save_btn.setFixedWidth(60)
        save_btn.clicked.connect(self._save)
        self._autoscroll = QCheckBox("Auto-scroll")
        self._autoscroll.setChecked(True)
        toolbar.addWidget(clear_btn)
        toolbar.addWidget(save_btn)
        toolbar.addWidget(self._autoscroll)
        toolbar.addStretch()
        layout.addLayout(toolbar)

        self._text = QTextEdit()
        self._text.setReadOnly(True)
        self._text.setStyleSheet(
            "background-color: #0d1117; color: #58a6ff; "
            "font-family: 'Courier New', monospace; font-size: 11px;"
        )
        layout.addWidget(self._text)

    def append(self, msg: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._text.append(f"<span style='color:#666'>[{ts}]</span> {msg}")
        if self._autoscroll.isChecked():
            sb = self._text.verticalScrollBar()
            sb.setValue(sb.maximum())

    def _clear(self) -> None:
        self._text.clear()

    def _save(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Save log", "coatheal_log.txt", "Text (*.txt)")
        if path:
            Path(path).write_text(self._text.toPlainText(), encoding="utf-8")


# ── main window ───────────────────────────────────────────────────
class MainWindow(QMainWindow):
    def __init__(self, default_host: str, default_tel_port: int, default_cmd_port: int):
        super().__init__()
        self.setWindowTitle("COATHEAL Ground Station")
        self.resize(1440, 900)

        self._receiver: Optional[TelemetryReceiver] = None
        self._sender = CommandSender(default_host, default_cmd_port)
        self._log_path = Path("logs/ground_telemetry.csv")
        self._pkt_count = 0
        self._onboard_addr = ""   # last addr seen from connection_changed(True, ...)

        pg.setConfigOption("background", "#0d1117")
        pg.setConfigOption("foreground", "#cccccc")

        self._build_central()
        self._build_left_dock(default_host, default_tel_port, default_cmd_port)
        self._build_bottom_dock()
        self._build_menus()
        self._build_status_bar()

        # Status bar heartbeat timer
        self._hb_timer = QTimer(self)
        self._hb_timer.setInterval(2000)
        self._hb_timer.timeout.connect(self._heartbeat_tick)
        self._hb_timer.start()
        self._last_pkt_time: float = 0.0

    # ── UI construction ────────────────────────────────────────────
    def _build_central(self) -> None:
        """Center: plot tabs on the left, values panel on the right."""
        splitter = QSplitter(Qt.Orientation.Horizontal)
        self.setCentralWidget(splitter)

        # Plot tabs
        self._tabs = QTabWidget()
        self._tabs.setTabPosition(QTabWidget.TabPosition.North)

        # Temperature plot
        self._temp_plot = LivePlotWidget("Temperature", "Temperature", "°C")
        self._temp_plot.add_curve("Box Temp", "#ff6b35", width=2)
        for i in range(9):
            self._temp_plot.add_curve(f"Sample {i}", HEATER_COLORS[i])
        self._tabs.addTab(self._temp_plot, "🌡 Temperature")

        # Pressure plot
        self._pres_plot = LivePlotWidget("Pressure", "Pressure", "mbar")
        self._pres_plot.add_curve("Pressure", "#3498db", width=2)
        self._tabs.addTab(self._pres_plot, "📈 Pressure")

        # Heater duties plot
        self._heat_plot = LivePlotWidget("Heater Duties", "Duty", "0–1")
        for i in range(10):
            self._heat_plot.add_curve(HEATER_LABELS[i], HEATER_COLORS[i])
        self._tabs.addTab(self._heat_plot, "🔥 Heaters")

        # Humidity + UV
        self._env_plot = LivePlotWidget("Environment", "Value", "")
        self._env_plot.add_curve("Humidity %", "#2ecc71")
        self._env_plot.add_curve("UV ×100", "#f1c40f")
        self._tabs.addTab(self._env_plot, "🌫 Env")

        splitter.addWidget(self._tabs)

        # Values panel (scrollable)
        self._values_panel = ValuesPanel()
        scroll = QScrollArea()
        scroll.setWidget(self._values_panel)
        scroll.setWidgetResizable(True)
        scroll.setMinimumWidth(180)
        splitter.addWidget(scroll)
        splitter.setSizes([1100, 280])

    def _build_left_dock(
        self, default_host: str, default_tel_port: int, default_cmd_port: int
    ) -> None:
        dock = QDockWidget("Control Panel", self)
        dock.setAllowedAreas(
            Qt.DockWidgetArea.LeftDockWidgetArea | Qt.DockWidgetArea.RightDockWidgetArea
        )
        dock.setFeatures(
            QDockWidget.DockWidgetFeature.DockWidgetMovable
            | QDockWidget.DockWidgetFeature.DockWidgetFloatable
        )

        container = QWidget()
        container.setFixedWidth(260)
        outer = QVBoxLayout(container)
        outer.setContentsMargins(4, 4, 4, 4)
        outer.setSpacing(8)

        # ── connection ─────────────────────────────────────────────
        conn_group = QGroupBox("Connection")
        conn_form  = QFormLayout(conn_group)
        conn_form.setSpacing(5)

        self._ip_edit       = QLineEdit(default_host)
        self._tel_port_edit = QLineEdit(str(default_tel_port))
        self._cmd_port_edit = QLineEdit(str(default_cmd_port))
        conn_form.addRow("Onboard IP:",  self._ip_edit)
        conn_form.addRow("Tel port:",    self._tel_port_edit)
        conn_form.addRow("Cmd port:",    self._cmd_port_edit)

        self._start_btn = QPushButton("▶  Start Telemetry")
        self._start_btn.setStyleSheet(
            "background-color: #27ae60; color: white; font-weight: bold; padding: 7px; border-radius: 3px;"
        )
        self._start_btn.clicked.connect(self._toggle_receiver)
        conn_form.addRow(self._start_btn)

        self._conn_lbl = QLabel("● Waiting for connection")
        self._conn_lbl.setStyleSheet("color: #888; font-size: 11px;")
        self._conn_lbl.setWordWrap(True)
        conn_form.addRow(self._conn_lbl)
        outer.addWidget(conn_group)

        # ── heater bars ────────────────────────────────────────────
        heat_group = QGroupBox("Heater Duties")
        heat_layout = QVBoxLayout(heat_group)
        heat_layout.setContentsMargins(4, 4, 4, 4)
        self._heater_panel = HeaterPanel()
        heat_layout.addWidget(self._heater_panel)
        outer.addWidget(heat_group)

        # ── commands ───────────────────────────────────────────────
        cmd_group = QGroupBox("Commands")
        cmd_layout = QVBoxLayout(cmd_group)
        cmd_layout.setContentsMargins(2, 2, 2, 2)
        self._cmd_panel = CommandPanel(self._sender)
        self._cmd_panel.log_message.connect(self._log_panel_append)
        cmd_scroll = QScrollArea()
        cmd_scroll.setWidget(self._cmd_panel)
        cmd_scroll.setWidgetResizable(True)
        cmd_layout.addWidget(cmd_scroll)
        outer.addWidget(cmd_group, stretch=1)

        dock.setWidget(container)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, dock)

    def _build_bottom_dock(self) -> None:
        dock = QDockWidget("Log", self)
        dock.setAllowedAreas(
            Qt.DockWidgetArea.BottomDockWidgetArea | Qt.DockWidgetArea.TopDockWidgetArea
        )
        self._log_panel = LogPanel()
        dock.setWidget(self._log_panel)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock)
        self.resizeDocks([dock], [200], Qt.Orientation.Vertical)

    def _build_menus(self) -> None:
        file_menu = self.menuBar().addMenu("File")

        open_csv = QAction("Open CSV log…", self)
        open_csv.triggered.connect(self._open_csv)
        file_menu.addAction(open_csv)

        clear_plots = QAction("Clear plots", self)
        clear_plots.setShortcut("Ctrl+L")
        clear_plots.triggered.connect(self._clear_plots)
        file_menu.addAction(clear_plots)

        file_menu.addSeparator()

        quit_act = QAction("Quit", self)
        quit_act.setShortcut("Ctrl+Q")
        quit_act.triggered.connect(self.close)
        file_menu.addAction(quit_act)

        view_menu = self.menuBar().addMenu("View")
        for tab_name in ("Temperature", "Pressure", "Heaters", "Environment"):
            act = QAction(tab_name, self)
            idx = ["Temperature", "Pressure", "Heaters", "Environment"].index(tab_name)
            act.triggered.connect(lambda _, i=idx: self._tabs.setCurrentIndex(i))
            view_menu.addAction(act)

    def _build_status_bar(self) -> None:
        sb = self.statusBar()
        self._sb_phase    = QLabel("Phase: —")
        self._sb_seq      = QLabel("SEQ: —")
        self._sb_pressure = QLabel("P: —")
        self._sb_temp     = QLabel("T: —")
        self._sb_link     = QLabel("LINK: —")
        self._sb_rate     = QLabel("0 pkt/s")

        for w in (self._sb_phase, self._sb_seq, self._sb_pressure,
                  self._sb_temp, self._sb_link, self._sb_rate):
            w.setStyleSheet("padding: 0 10px; font-family: monospace;")
            sb.addPermanentWidget(w)

        self._sb_sep = QLabel("|")
        self._sb_sep.setStyleSheet("color: #444;")
        sb.addPermanentWidget(self._sb_sep)

    # ── receiver control ───────────────────────────────────────────
    def _toggle_receiver(self) -> None:
        if self._receiver is not None and self._receiver.isRunning():
            self._receiver.stop()
            self._receiver.wait(3000)
            self._receiver = None
            self._start_btn.setText("▶  Start Telemetry")
            self._start_btn.setStyleSheet(
                "background-color: #27ae60; color: white; font-weight: bold; "
                "padding: 7px; border-radius: 3px;"
            )
            self._conn_lbl.setText("● Stopped")
            self._conn_lbl.setStyleSheet("color: #888; font-size: 11px;")
            return

        try:
            tel_port = int(self._tel_port_edit.text())
            cmd_port = int(self._cmd_port_edit.text())
        except ValueError:
            QMessageBox.warning(self, "Bad input", "Ports must be integers.")
            return

        host = self._ip_edit.text().strip()
        self._sender = CommandSender(host, cmd_port)
        self._cmd_panel.set_sender(self._sender)

        self._receiver = TelemetryReceiver("0.0.0.0", tel_port, self._log_path)
        self._receiver.packet_received.connect(self._on_packet)
        self._receiver.log_message.connect(self._log_panel_append)
        self._receiver.connection_changed.connect(self._on_connection_changed)
        self._receiver.start()

        self._start_btn.setText("⏹  Stop Telemetry")
        self._start_btn.setStyleSheet(
            "background-color: #c0392b; color: white; font-weight: bold; "
            "padding: 7px; border-radius: 3px;"
        )
        self._log_panel_append(f"[telemetry] receiver started — cmd target: {host}:{cmd_port}")

    @pyqtSlot(bool, str)
    def _on_connection_changed(self, connected: bool, addr: str) -> None:
        if connected:
            self._onboard_addr = addr
            self._conn_lbl.setText(f"● {addr}")
            self._conn_lbl.setStyleSheet(
                "color: #2ecc71; font-weight: bold; font-size: 11px;"
            )
        else:
            self._onboard_addr = ""
            self._conn_lbl.setText("● Waiting for onboard…")
            self._conn_lbl.setStyleSheet("color: #e74c3c; font-size: 11px;")
            # Clear status bar so stale values don't look like live data
            for w in (self._sb_phase, self._sb_seq, self._sb_pressure,
                      self._sb_temp, self._sb_link, self._sb_rate):
                w.setStyleSheet("padding: 0 10px; font-family: monospace; color: #555;")
            self._sb_link.setText("LINK: —")
            self._sb_rate.setText("disconnected")

    # ── packet handler (main thread via signal) ────────────────────
    @pyqtSlot(object)
    def _on_packet(self, pkt: TelemetryPacket) -> None:
        self._pkt_count += 1
        self._last_pkt_time = time.monotonic()
        seq = pkt.seq

        # Receiving a packet is ground truth that we're connected.
        # Correct the label if it's stale (e.g. due to signal ordering on reconnect).
        if "Waiting" in self._conn_lbl.text():
            addr = self._onboard_addr or self._ip_edit.text().strip()
            self._conn_lbl.setText(f"● {addr}")
            self._conn_lbl.setStyleSheet(
                "color: #2ecc71; font-weight: bold; font-size: 11px;"
            )

        # Temperature plot
        temp_vals: dict[str, float] = {"Box Temp": pkt.box_temp_c}
        for i, t in enumerate(pkt.sample_temps_c[:9]):
            temp_vals[f"Sample {i}"] = t
        self._temp_plot.push(seq, temp_vals)

        # Pressure
        self._pres_plot.push(seq, {"Pressure": pkt.ambient_pressure_mbar})

        # Heater duties
        heat_vals = {HEATER_LABELS[i]: d for i, d in enumerate(pkt.heater_duty[:10])}
        self._heat_plot.push(seq, heat_vals)

        # Environment
        self._env_plot.push(seq, {
            "Humidity %": pkt.ambient_humidity_pct,
            "UV ×100":    pkt.uv * 100.0,
        })

        # Heater bars
        self._heater_panel.update_duties(pkt.heater_duty)

        # Values panel
        self._values_panel.update_packet(pkt)

        # Status bar
        color = phase_color(pkt.phase)
        self._sb_phase.setText(f"Phase: {pkt.phase}")
        self._sb_phase.setStyleSheet(f"padding: 0 10px; font-family: monospace; color: {color}; font-weight: bold;")
        self._sb_seq.setText(f"SEQ: {seq}")
        self._sb_pressure.setText(f"P: {pkt.ambient_pressure_mbar:.1f} mbar")
        self._sb_temp.setText(f"T: {pkt.box_temp_c:.2f} °C")

        link_ok = "LINK_OK" in pkt.status
        self._sb_link.setText("LINK: OK" if link_ok else "LINK: FAIL")
        self._sb_link.setStyleSheet(
            f"padding: 0 10px; font-family: monospace; "
            f"color: {'#2ecc71' if link_ok else '#e74c3c'};"
        )

        # Log
        self._log_panel_append(
            f"<span style='color:{color};font-weight:bold'>{pkt.phase}</span> "
            f"seq={seq} "
            f"P=<b>{pkt.ambient_pressure_mbar:.1f}</b>mbar "
            f"Tbox=<b>{pkt.box_temp_c:.2f}</b>°C "
            f"{pkt.status}"
        )

    def _heartbeat_tick(self) -> None:
        if self._last_pkt_time > 0:
            age = time.monotonic() - self._last_pkt_time
            if age > 5.0:
                self._sb_rate.setText(f"⚠ {age:.0f}s stale")
                self._sb_rate.setStyleSheet("padding: 0 10px; color: #e74c3c;")
            else:
                self._sb_rate.setText("● live")
                self._sb_rate.setStyleSheet("padding: 0 10px; color: #2ecc71;")

    # ── helpers ────────────────────────────────────────────────────
    def _log_panel_append(self, msg: str) -> None:
        self._log_panel.append(msg)

    def _clear_plots(self) -> None:
        for plot in (self._temp_plot, self._pres_plot, self._heat_plot, self._env_plot):
            plot.clear()
        self._pkt_count = 0

    def _open_csv(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Open telemetry CSV", "logs/", "CSV (*.csv)"
        )
        if not path:
            return
        self._clear_plots()
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    try:
                        seq      = int(row["seq"])
                        box_temp = float(row["box_temp_c"])
                        pressure = float(row["ambient_pressure_mbar"])
                        humidity = float(row["ambient_humidity_pct"])
                        uv       = float(row["uv"])
                        duties   = [float(x) for x in row["heater_duty"].split("|") if x]
                        samples  = [float(x) for x in row["sample_temps_c"].split("|") if x]

                        temp_vals: dict[str, float] = {"Box Temp": box_temp}
                        for i, t in enumerate(samples[:9]):
                            temp_vals[f"Sample {i}"] = t
                        self._temp_plot.push(seq, temp_vals)
                        self._pres_plot.push(seq, {"Pressure": pressure})
                        heat_vals = {HEATER_LABELS[i]: d for i, d in enumerate(duties[:10])}
                        self._heat_plot.push(seq, heat_vals)
                        self._env_plot.push(seq, {
                            "Humidity %": humidity,
                            "UV ×100":    uv * 100.0,
                        })
                    except (KeyError, ValueError):
                        continue
            self._log_panel_append(f"[csv] loaded {path}")
        except OSError as exc:
            QMessageBox.critical(self, "Error", f"Could not open file:\n{exc}")

    def closeEvent(self, event) -> None:
        if self._receiver is not None:
            self._receiver.stop()
            self._receiver.wait(3000)
        event.accept()


# ── entry point ────────────────────────────────────────────────────
def main() -> None:
    parser = argparse.ArgumentParser(description="COATHEAL Ground Station GUI")
    parser.add_argument("--host",     default="169.254.10.10")
    parser.add_argument("--tel-port", type=int, default=4000)
    parser.add_argument("--cmd-port", type=int, default=5000)
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setApplicationName("COATHEAL Ground Station")
    app.setStyle("Fusion")

    # Dark Fusion palette
    pal = QPalette()
    pal.setColor(QPalette.ColorRole.Window,           QColor("#1c1c1c"))
    pal.setColor(QPalette.ColorRole.WindowText,       QColor("#dddddd"))
    pal.setColor(QPalette.ColorRole.Base,             QColor("#141414"))
    pal.setColor(QPalette.ColorRole.AlternateBase,    QColor("#1c1c1c"))
    pal.setColor(QPalette.ColorRole.ToolTipBase,      QColor("#2a2a2a"))
    pal.setColor(QPalette.ColorRole.ToolTipText,      QColor("#dddddd"))
    pal.setColor(QPalette.ColorRole.Text,             QColor("#dddddd"))
    pal.setColor(QPalette.ColorRole.Button,           QColor("#2a2a2a"))
    pal.setColor(QPalette.ColorRole.ButtonText,       QColor("#dddddd"))
    pal.setColor(QPalette.ColorRole.BrightText,       QColor("#ffffff"))
    pal.setColor(QPalette.ColorRole.Highlight,        QColor("#3d6b99"))
    pal.setColor(QPalette.ColorRole.HighlightedText,  QColor("#ffffff"))
    pal.setColor(QPalette.ColorRole.Link,             QColor("#5dade2"))
    pal.setColor(QPalette.ColorRole.Dark,             QColor("#111111"))
    pal.setColor(QPalette.ColorRole.Mid,              QColor("#222222"))
    pal.setColor(QPalette.ColorRole.Shadow,           QColor("#000000"))
    app.setPalette(pal)

    win = MainWindow(args.host, args.tel_port, args.cmd_port)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
