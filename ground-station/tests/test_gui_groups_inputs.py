"""Headless tests for the owner list of 2026-09-15 in the window: motor
groups from GET_LAYOUT, the >40 °C heating confirmation, SI motion units,
and the input rules for number boxes (wheel only with Ctrl/Shift, "."
decimals, units outside the box)."""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui_helpers import capture_sends, frame, make_window  # noqa: E402

SESSION = "coatheal-1787760547-1"
# Three specimens on motor 0 (S2 unheated), five on motor 1 (S7 unheated);
# H2 reads S3, the second click reads S3.
UNEVEN_REPLY = ("samples=8;heaters=6;motor0=0,1,2;motor1=3,4,5,6,7;heater_samples=0,1,3,4,5,6;"
                "clicks=0,3;rtd_channels=1,2,3,4,5,6,7,8;heater_lines=19,13,6,5,24,23")


class WindowTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as exc:
            raise unittest.SkipTest(f"no Qt platform: {exc}")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.win = make_window(Path(self._tmp.name))

    def tearDown(self) -> None:
        self.win.close()
        self._tmp.cleanup()

    def feed(self, **kw) -> None:
        """One live frame, logged the way the telemetry receiver logs it."""
        from app.protocol import parse_telemetry_csv
        pkt = parse_telemetry_csv(frame(**kw))
        self.win._logs.on_packet(pkt)
        self.win._on_packet(pkt)

    def reply(self, cmd: str, body: str = "", *, ok: bool = True, error: str = "",
              raw: str | None = None, tag=None) -> None:
        from app.protocol import CommandResponse
        verb = cmd.split()[0]
        if raw is None:
            raw = f"ACK,{verb},{body}" if ok else f"NACK,{verb},{error}"
        self.win._dispatcher.response_received.emit(
            cmd, CommandResponse(ok=ok, command=verb, body=body, error=error, raw=raw), 5.0,
            self.win if tag is None else tag)

    def events(self) -> str:
        return "\n".join(self.win._events.lines())


class MotorGroupTests(WindowTestCase):
    def test_layout_is_asked_once_per_onboard_session(self) -> None:
        sent = capture_sends(self.win._dispatcher, layout_query=True)
        self.feed(seq=1)
        self.feed(seq=2)
        self.assertEqual(sent.count("GET_LAYOUT"), 1)
        self.feed(session="coatheal-1787761000-2", seq=1)
        self.feed(session="coatheal-1787761000-2", seq=2)
        self.assertEqual(sent.count("GET_LAYOUT"), 2, "a restarted onboard (new session) is asked again")
        self.reply("RADIO_SILENCE", "radio silent", tag=self.win._system)
        self.feed(session="coatheal-1787761100-3", seq=1)
        self.assertEqual(sent.count("GET_LAYOUT"), 2, "nothing is asked during radio silence")
        self.reply("RADIO_RESUME", "radio resumed", tag=self.win._system)
        self.feed(session="coatheal-1787761100-3", seq=2)
        self.assertEqual(sent.count("GET_LAYOUT"), 3, "asked on the first frame after the radio is back")

    def test_unanswered_layout_query_is_asked_again_later(self) -> None:
        from app.gui.main_window import LAYOUT_RETRY_S
        sent = capture_sends(self.win._dispatcher, layout_query=True)
        self.feed(seq=1)
        self.reply("GET_LAYOUT", ok=False, error="timed out", raw="")
        self.assertIn("no reply", self.events())
        self.feed(seq=2)
        self.assertEqual(sent.count("GET_LAYOUT"), 1, "not re-asked on every frame")
        real = time.monotonic
        with mock.patch("app.gui.main_window.time.monotonic", side_effect=lambda: real() + LAYOUT_RETRY_S + 1):
            self.feed(seq=3)
        self.assertEqual(sent.count("GET_LAYOUT"), 2)

    # MUTATION: drop `self._layout_session = ""` from the no-reply branch of
    # MainWindow._absorb_layout and confirm this test fails on the last count.

    def test_old_firmware_keeps_the_schematic_groups(self) -> None:
        from app.gui.state import DEFAULT_LAYOUT
        sent = capture_sends(self.win._dispatcher, layout_query=True)
        self.feed(seq=1)
        self.reply("GET_LAYOUT", ok=False, error="unknown command")
        self.feed(seq=2)
        self.assertEqual(sent.count("GET_LAYOUT"), 1, "a NACK is an answer: not asked again this session")
        self.assertIs(self.win._layout, DEFAULT_LAYOUT)
        self.assertIn("does not report its motor groups", self.events())
        self.assertIn("Schematic groups", self.win._thermal.layout_note.text())
        self.reply("GET_LAYOUT", "samples=8;heaters=6;motor0=0,1")
        self.assertIs(self.win._layout, DEFAULT_LAYOUT)
        self.assertIn("unreadable GET_LAYOUT reply", self.events())

    def test_layout_reply_regroups_every_motor_view(self) -> None:
        from PyQt6.QtWidgets import QLabel, QWidget
        win = self.win
        self.feed(seq=1)
        self.reply("GET_LAYOUT", UNEVEN_REPLY)
        self.assertTrue(win._layout.reported)
        self.assertIn("motor groups from the onboard", self.events())

        thermal = win._thermal
        groups = [thermal.findChildren(QWidget, f"motorGroup{m}") for m in range(2)]
        self.assertEqual([len(g) for g in groups], [1, 1], "the old group boxes are gone at once")
        g0, g1 = groups[0][0], groups[1][0]
        self.assertEqual([h for h in range(6) if g0.isAncestorOf(thermal.rows[h])], [0, 1])
        self.assertEqual([h for h in range(6) if g1.isAncestorOf(thermal.rows[h])], [2, 3, 4, 5])
        self.assertEqual(sorted(thermal.sample_rows), [2, 7])
        self.assertTrue(g0.isAncestorOf(thermal.sample_rows[2]))
        self.assertTrue(g1.isAncestorOf(thermal.sample_rows[7]))
        self.assertIn("3 specimens, 2 heated", thermal.group_titles[0].text())
        self.assertIn("5 specimens, 4 heated", thermal.group_titles[1].text())
        self.assertIn("as the onboard reports", thermal.layout_note.text())

        # Rows show and gate on the sample their heater reads (H2 -> S3).
        self.feed(seq=2)
        self.assertEqual(thermal.rows[2].measured.text(), "S3   4.0")
        self.assertEqual(thermal.sample_rows[7].measured.text(), "S7   8.0")
        self.feed(seq=3, samples="1,2,3,nan,5,6,7,8",
                  valid="AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:0|S4:1|S5:1|S6:1|S7:1")
        self.assertIn("S3", thermal.rows[2].btn_set.reason() or "")
        self.assertIsNone(thermal.rows[3].btn_set.reason(), "H3 reads S4, which is valid")

        self.assertEqual(win._motion.cards[0].group.text(), "S0 S1 S2 · H0 H1")
        self.assertEqual(win._motion.cards[1].group.text(), "S3 S4 S5 S6 S7 · H2 H3 H4 H5")

        labels = [label.text() for label in win._values.findChildren(QLabel)]
        for text in ("Motor 0 group", "Motor 1 group", "S2 °C · unheated", "S3 °C · H2",
                     "S0 Ω · click 1", "S3 Ω · click 2", "H5 duty"):
            self.assertEqual(labels.count(text), 1, text)
        self.assertNotIn("S4 Ω · click 2", labels)
        self.assertEqual(win._values._fields["resistance_3"].text(), "—", "S3 carries no resistance in the frame")

        temps = [plot.series_names() for plot in win._plots.temps.plots]
        self.assertEqual(temps[0], ["S0", "T0", "S1", "T1", "S2"])
        self.assertEqual(temps[1], ["S3", "T2", "S4", "T3", "S5", "T4", "S6", "T5", "S7"])
        self.assertEqual([plot.series_names() for plot in win._plots.heaters.plots],
                         [["H0", "H1"], ["H2", "H3", "H4", "H5"]])

        meta = json.loads((win._logs.dir_for(SESSION) / "session.json").read_text(encoding="utf-8"))
        self.assertEqual(meta["layout"]["motor_samples"], [[0, 1, 2], [3, 4, 5, 6, 7]])
        self.assertEqual(meta["layout"]["heater_samples"], [0, 1, 3, 4, 5, 6])
        self.assertEqual(meta["layout"]["clicks"], [0, 3])

        # The same answer again rearranges nothing.
        self.reply("GET_LAYOUT", UNEVEN_REPLY)
        self.assertEqual(self.events().count("motor groups from the onboard"), 1)

    # MUTATION: remove `self._plots.set_layout(layout)` from
    # MainWindow.set_layout and confirm the plot series assertions fail.


class HeatingConfirmationTests(WindowTestCase):
    def test_heater_row_asks_above_40(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.feed()
        thermal = self.win._thermal
        row = thermal.rows[0]
        row.target.setValue(40.0)
        with mock.patch("app.gui.tab_thermal.confirm", return_value=True) as ask:
            row.btn_set.click()
        self.assertEqual(ask.call_count, 0, "40 °C itself does not ask")
        self.assertEqual(sent, ["SET_TEMP_TARGET 0 40.000"])
        row.target.setValue(45.5)
        with mock.patch("app.gui.tab_thermal.confirm", return_value=False) as ask:
            row.btn_set.click()
        self.assertEqual(ask.call_count, 1)
        self.assertIn("45.5 °C", ask.call_args.args[2])
        self.assertEqual(sent, ["SET_TEMP_TARGET 0 40.000"], "declined: nothing sent")
        self.assertIn("not sent", thermal.resp_heaters.text())
        with mock.patch("app.gui.tab_thermal.confirm", return_value=True):
            row.btn_set.click()
        self.assertEqual(sent[-1], "SET_TEMP_TARGET 0 45.500")

    # MUTATION: make ThermalTab._confirmed return True without asking and
    # confirm test_heater_row_asks_above_40 fails on the declined send.

    def test_set_all_autotune_and_presets_ask_above_40(self) -> None:
        from app.thermal_presets import ThermalPreset
        sent = capture_sends(self.win._dispatcher)
        self.feed()
        thermal = self.win._thermal
        thermal.all_target.setValue(50.0)
        thermal.tune_heater.setCurrentIndex(1)
        thermal.tune_setpoint.setValue(60.0)
        with mock.patch("app.gui.tab_thermal.confirm", return_value=False) as ask:
            thermal.btn_set_all.click()
            thermal._tune_start()
        self.assertEqual(ask.call_count, 2)
        self.assertEqual(sent, [])
        self.assertEqual([row.target.value() for row in thermal.rows], [20.0] * 6,
                         "a declined Set all leaves the heater boxes alone")
        self.assertFalse(thermal._tune_timer.isActive())

        thermal.presets.put(ThermalPreset("hot", targets_c=[30.0, 55.0, None, None, None, None]))
        thermal.refresh_presets()
        thermal.preset_select.setCurrentText("hot")
        with mock.patch("app.gui.tab_thermal.confirm", return_value=False) as ask:
            thermal.btn_apply_preset.click()
        self.assertEqual(sent, [])
        self.assertIn("H1 55 °C", ask.call_args.args[2])
        self.assertNotIn("H0", ask.call_args.args[2], "only the targets above 40 °C are named")
        with mock.patch("app.gui.tab_thermal.confirm", return_value=True):
            thermal.btn_apply_preset.click()
        self.assertIn("SET_TEMP_TARGET 1 55.000", sent)

    def test_console_asks_above_40(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        entry = self.win._console.entry
        with mock.patch("app.gui.main_window.confirm", return_value=False) as ask:
            entry.setText("set_all_temp_targets 65")
            entry._submit()
        self.assertEqual(ask.call_count, 1)
        self.assertEqual(sent, [])
        self.assertIn("not sent (not confirmed)", self.events())
        with mock.patch("app.gui.main_window.confirm", return_value=True) as ask:
            entry.setText("SET_TEMP_TARGET 3 41")
            entry._submit()
            entry.setText("SET_TEMP_TARGET 3 39")
            entry._submit()
        self.assertEqual(ask.call_count, 1, "39 °C does not ask")
        self.assertEqual(sent, ["SET_TEMP_TARGET 3 41", "SET_TEMP_TARGET 3 39"])


class SiUnitTests(WindowTestCase):
    def test_motion_values_and_sequences_in_mm(self) -> None:
        sent = capture_sends(self.win._dispatcher)
        self.feed()   # hz:100, us:4 on both motors
        self.assertEqual(self.win._values._fields["m0_cfg"].text(), "1.00 mm/s · µstep 1/4")
        advanced = self.win._advanced
        advanced.seq_name.setText("bend1")
        advanced.seq_table.item(0, 0).setText("1.5")
        advanced.seq_table.item(0, 1).setText("5")
        advanced.seq_table.item(0, 2).setText("0,125")     # a typed comma reads as "."
        advanced._seq_load()
        self.assertEqual(sent[-1], "BENDSEQ_LOAD 0 bend1 600:5:12.5")
        advanced.seq_table.item(0, 2).setText("0.6")
        advanced._seq_load()
        self.assertEqual(len(sent), 1, "above 0.5 mm/s is refused")
        self.assertIn("(0, 0.5] mm/s", advanced.resp_seq.text().replace("​", ""))
        target, hold, speed, _btn = advanced.plan_rows[1]
        target.setValue(2.0); hold.setValue(5.0); speed.setValue(0.29)
        advanced._plan_load(1)
        self.assertEqual(sent[-1], "FALLBACK_PLAN 1 800 5 29")


class InputRuleTests(WindowTestCase):
    def wheel(self, widget, modifiers=None, notches: int = 1) -> None:
        from PyQt6.QtCore import QPoint, QPointF, Qt
        from PyQt6.QtGui import QWheelEvent
        from PyQt6.QtWidgets import QApplication
        centre = widget.rect().center()
        event = QWheelEvent(QPointF(centre), QPointF(widget.mapToGlobal(centre)), QPoint(0, 0),
                            QPoint(0, 120 * notches), Qt.MouseButton.NoButton,
                            modifiers if modifiers is not None else Qt.KeyboardModifier.NoModifier,
                            Qt.ScrollPhase.NoScrollPhase, False)
        QApplication.sendEvent(widget, event)

    def test_units_sit_outside_the_boxes(self) -> None:
        from PyQt6.QtWidgets import QAbstractSpinBox, QLabel
        boxes = self.win.findChildren(QAbstractSpinBox)
        self.assertGreater(len(boxes), 10)
        for box in boxes:
            self.assertEqual(getattr(box, "suffix", lambda: "")(), "", f"{type(box).__name__} {box.text()!r}")
        labels = {label.text() for label in self.win._motion.findChildren(QLabel)}
        for unit in ("mm/s", "mm/s²", "mm", "s", "A"):
            self.assertIn(unit, labels)
        self.assertEqual(self.win._motion.speed.maximum(), 0.5)
        thermal_units = [label for label in self.win._thermal.findChildren(QLabel) if label.text() == "°C"]
        self.assertGreaterEqual(len(thermal_units), 6 + 2, "each heater row, Set all and autotune")

    def test_decimal_point_whatever_the_system_locale(self) -> None:
        from PyQt6.QtCore import QLocale
        from PyQt6.QtTest import QTest
        from app.gui.widgets import apply_number_locale
        QLocale.setDefault(QLocale(QLocale.Language.German, QLocale.Country.Germany))
        other = tempfile.TemporaryDirectory()
        try:
            win = make_window(Path(other.name))
            try:
                speed = win._motion.speed
                self.assertEqual(QLocale().decimalPoint(), ".")
                speed.setValue(0.25)
                self.assertNotIn(",", speed.text())
                self.assertIn("0.25", speed.text())
                speed.lineEdit().selectAll()
                QTest.keyClicks(speed.lineEdit(), "0,35")
                speed.interpretText()
                self.assertAlmostEqual(speed.value(), 0.35, places=6)
            finally:
                win.close()
        finally:
            other.cleanup()
            apply_number_locale()

    # MUTATION: remove apply_number_locale() from MainWindow.__init__ and
    # confirm test_decimal_point_whatever_the_system_locale fails.

    def test_wheel_edits_only_with_ctrl_or_shift(self) -> None:
        from PyQt6.QtCore import Qt
        from app.gui.tab_thermal import ThermalTab
        tab = ThermalTab(self.win._dispatcher)
        tab.resize(440, 260)
        tab.show()
        try:
            self._app.processEvents()
            scroll = tab.verticalScrollBar()
            self.assertGreater(scroll.maximum(), 0, "the tab must be taller than its view")
            box = tab.rows[0].target
            before = box.value()
            self.wheel(box, notches=-1)
            self.assertEqual(box.value(), before, "a plain wheel does not edit the box")
            self.assertGreater(scroll.value(), 0, "a plain wheel scrolls the page instead")
            self.wheel(box, Qt.KeyboardModifier.ControlModifier)
            self.assertEqual(box.value(), before + box.singleStep())
            self.wheel(box, Qt.KeyboardModifier.ShiftModifier, notches=-2)
            self.assertEqual(box.value(), before - box.singleStep())
            combo = tab.tune_heater
            combo.setCurrentIndex(2)
            self.wheel(combo)
            self.assertEqual(combo.currentIndex(), 2)
            self.wheel(combo, Qt.KeyboardModifier.ControlModifier)
            self.assertEqual(combo.currentIndex(), 1)
        finally:
            tab.close()
            tab.deleteLater()

    # MUTATION: make InputPolicy.eventFilter return False for wheel events
    # and confirm test_wheel_edits_only_with_ctrl_or_shift fails on the
    # plain-wheel assertion.


if __name__ == "__main__":
    unittest.main()
