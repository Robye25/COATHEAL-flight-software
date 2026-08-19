"""Headless GUI tests for Task 3 of the GS UI/UX pass: safety-critical
Esc/STOP MOTORS behavior, the failed-receiver link-state fix, and the
layout/button/tooltip changes (ModePanel 2x2, collapsible bend-sequence
editor, View menu, `Sends:` tooltips, resize-to-fit).

Follows the conventions of test_gui_smoke.py / test_gui_health.py:
offscreen QPA platform, skip cleanly if PyQt6/Qt aren't usable, build a
real MainWindow, and prefer capturing CommandDispatcher.send() calls over
letting them hit real sockets.
"""
from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def _pump_until(app, predicate, timeout_s: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


def _find_button(container, text: str):
    from PyQt6.QtWidgets import QPushButton
    for b in container.findChildren(QPushButton):
        if b.text() == text:
            return b
    return None


class GuiUiUxTests(unittest.TestCase):
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

    def _make_window(self, tel_port: int, cmd_port: int, tag: str):
        from app.gui.main_window import MainWindow
        return MainWindow(
            bind="127.0.0.1", tel_port=tel_port, cmd_port=cmd_port,
            cmd_host="127.0.0.1", log_path=Path(f"logs/uiux_{tag}.csv"),
            firewall_check=False,
        )

    @staticmethod
    def _capture_sends(dispatcher) -> list:
        sent: list = []
        dispatcher.send = lambda cmd, tag=None, timeout=3.0: sent.append(cmd)
        return sent

    # ── 1. Esc stops BOTH motors ──
    def test_esc_stops_both_motors(self) -> None:
        from PyQt6.QtGui import QKeySequence, QShortcut

        win = self._make_window(44200, 45200, "esc")
        try:
            sent = self._capture_sends(win._dispatcher)
            esc = None
            for s in win.findChildren(QShortcut):
                if s.key() == QKeySequence("Esc"):
                    esc = s
                    break
            self.assertIsNotNone(esc, "no Esc QShortcut registered")
            esc.activated.emit()
            self.assertEqual(
                sent, ["STEPPER_STOP 0", "STEPPER_STOP 1"],
                "Esc must dispatch STEPPER_STOP for both motor ids, in order",
            )
        finally:
            win.close()

    # MUTATION: change `sc("Esc", self._stepper_panel.emergency_stop_all)`
    # back to `sc("Esc", self._stepper_panel.emergency_stop)` (single
    # selected motor) and confirm test_esc_stops_both_motors fails --
    # `sent` becomes `["STEPPER_STOP 0"]` (only the combo box's default
    # selection), not both ids.

    # ── 2. EmergencyBar STOP MOTORS: both stops, no confirmation dialog ──
    def test_emergency_bar_stop_motors_sends_both_no_dialog(self) -> None:
        import app.gui.panels_control as pc

        win = self._make_window(44201, 45201, "stopmotors")
        try:
            sent = self._capture_sends(win._dispatcher)
            original_confirm = pc.confirm

            def _no_confirm_allowed(*_args, **_kwargs):
                raise AssertionError("STOP MOTORS must not prompt for confirmation")
            pc.confirm = _no_confirm_allowed
            try:
                win._emergency._stop_motors_btn.click()
            finally:
                pc.confirm = original_confirm
            self.assertEqual(sent, ["STEPPER_STOP 0", "STEPPER_STOP 1"])
        finally:
            win.close()

    # MUTATION: add `needs_confirm=True`-style gating in front of the
    # STOP MOTORS button's click handler (wrap the dispatch call with an
    # `if not confirm(...): return`) and confirm
    # test_emergency_bar_stop_motors_sends_both_no_dialog fails with the
    # injected AssertionError ("must not prompt").

    # ── 3. failed receiver status clears _link_ok ──
    def test_receiver_failed_state_clears_link_ok(self) -> None:
        win = self._make_window(44202, 45202, "failedlink")
        try:
            listening = _pump_until(
                self._app, lambda: not win._connection._start_btn.isEnabled()
            )
            self.assertTrue(listening, "live receiver never reported listening")
            live_receiver = win._receiver
            self.assertIsNotNone(live_receiver)

            win._link_ok = True
            live_receiver.status_changed.emit("failed")

            self.assertFalse(
                win._link_ok,
                "the failed branch must defensively clear _link_ok itself, "
                "since it also nulls self._receiver, which drops any late "
                "connection_changed(False) from the dying receiver",
            )
            self.assertIn("failed", win._connection._status.text().lower(),
                          "status label must still say *why* it failed")
        finally:
            live_receiver.stop()
            live_receiver.wait(2000)
            win.close()

    # MUTATION: delete `self._link_ok = False` from the `state == "failed"`
    # branch in main_window.py's _on_receiver_status and confirm
    # test_receiver_failed_state_clears_link_ok fails (`_link_ok` stays
    # True).

    # ── 4. ModePanel ARM row is a 2x2 grid ──
    def test_mode_panel_arm_row_is_2x2_grid(self) -> None:
        win = self._make_window(44203, 45203, "modegrid")
        try:
            mp = win._mode_panel
            grid = mp._arm_grid
            expected = {
                (0, 0): mp._btn_arm,
                (0, 1): mp._btn_dis,
                (1, 0): mp._btn_start,
                (1, 1): mp._btn_stop,
            }
            for (row, col), btn in expected.items():
                idx = grid.indexOf(btn)
                self.assertGreaterEqual(idx, 0, f"{btn.text()} not in the ARM grid")
                r, c, _rspan, _cspan = grid.getItemPosition(idx)
                self.assertEqual((r, c), (row, col),
                                  f"{btn.text()} should be at grid ({row},{col})")
        finally:
            win.close()

    # MUTATION: swap FORCE START and FORCE STOP's `pos=` arguments (put
    # FORCE STOP at (1, 0) and FORCE START at (1, 1)) and confirm
    # test_mode_panel_arm_row_is_2x2_grid fails, naming the wrong button
    # at (1, 0).

    # ── 5. Bend sequence editor: collapsed by default, expands ──
    def test_bend_sequence_collapsed_by_default_and_expands(self) -> None:
        win = self._make_window(44204, 45204, "bendcollapse")
        try:
            sp = win._stepper_panel
            self.assertFalse(sp.sequence_expanded(), "must be collapsed by default")
            self.assertTrue(sp._seq_body.isHidden(), "body must be hidden while collapsed")

            sp._seq_group.setChecked(True)

            self.assertTrue(sp.sequence_expanded())
            self.assertFalse(sp._seq_body.isHidden(), "body must show once expanded")
        finally:
            win.close()

    # MUTATION: change `self._seq_group.setChecked(False)` to
    # `setChecked(True)` in StepperPanel.__init__ and confirm
    # test_bend_sequence_collapsed_by_default_and_expands fails on the
    # "must be collapsed by default" assertion.

    # ── 6. View menu toggles dock visibility ──
    def test_view_menu_toggles_dock_visibility(self) -> None:
        from PyQt6.QtWidgets import QMenu

        win = self._make_window(44205, 45205, "viewmenu")
        try:
            # Dock visibility bookkeeping (isHidden()/toggleViewAction's
            # checked state) only tracks real show/hide transitions once
            # the top-level window has actually been shown at least once
            # -- before that, docks report isVisible()=False (inherited
            # from the unshown top-level) without ever being "hidden" in
            # the explicit sense toggleViewAction toggles.
            win.show()
            self._app.processEvents()
            view_menu = None
            for m in win.menuBar().findChildren(QMenu):
                if m.title().replace("&", "") == "View":
                    view_menu = m
                    break
            self.assertIsNotNone(view_menu, "no View menu found on the menu bar")

            actions = view_menu.actions()
            docks = (win._left_dock, win._right_dock, win._bottom_dock)
            self.assertEqual(len(actions), len(docks))
            for dock in docks:
                self.assertIn(dock.toggleViewAction(), actions,
                              f"{dock.windowTitle()}'s toggleViewAction missing from View menu")

            left_action = win._left_dock.toggleViewAction()
            self.assertFalse(win._left_dock.isHidden(), "left dock should start visible")
            left_action.trigger()
            self.assertTrue(win._left_dock.isHidden(),
                            "triggering the View menu action must hide the dock")
            left_action.trigger()
            self.assertFalse(win._left_dock.isHidden(),
                             "triggering it again must re-show the dock")
        finally:
            win.hide()
            win.close()

    # MUTATION: comment out the `for dock in (...): view_menu.addAction(...)`
    # loop body in main_window.py's _build_menus and confirm
    # test_view_menu_toggles_dock_visibility fails on "no View menu found"
    # or the action-count assertion.

    # ── 7. Sample of >=5 command buttons carry accurate "Sends:" tooltips ──
    def test_sample_command_buttons_have_sends_tooltips(self) -> None:
        win = self._make_window(44206, 45206, "tooltips")
        try:
            checks = [
                (win._command_panel, "PING", "Sends: PING"),
                (win._command_panel, "STATUS", "Sends: STATUS"),
                (win._command_panel, "CHECK", "Sends: CHECK"),
                (win._command_panel, "COMPONENTS", "Sends: COMPONENTS"),
                (win._stepper_panel, "ENABLE", "Sends: STEPPER_ENABLE <motor_id>"),
                (win._stepper_panel, "SET ZERO", "Sends: SET_POSITION_ZERO <motor_id>"),
                (win._mode_panel, "ARM", "Sends: ARM"),
                (win._emergency, "HEATERS OFF", "Sends: HEATERS_OFF"),
            ]
            for container, text, expected in checks:
                btn = _find_button(container, text)
                self.assertIsNotNone(btn, f"button {text!r} not found")
                self.assertIn(
                    expected, btn.toolTip(),
                    f"{text!r} tooltip should contain {expected!r}, got {btn.toolTip()!r}",
                )
        finally:
            win.close()

    # MUTATION: change the STATUS button's tooltip string in
    # panels_control.py's CommandPanel from "Sends: STATUS" to
    # "Sends: STAT" and confirm test_sample_command_buttons_have_sends_tooltips
    # fails naming the STATUS button.

    # ── 8. Resize smoke: window fits at 1280x720 and 1920x1080 ──
    def test_window_fits_at_1280x720_and_1920x1080(self) -> None:
        win = self._make_window(44207, 45207, "resize")
        try:
            win.show()
            self._app.processEvents()
            for w, h in ((1280, 720), (1920, 1080)):
                win.resize(w, h)
                self._app.processEvents()
                self.assertEqual(
                    (win.width(), win.height()), (w, h),
                    f"window did not honor resize to {w}x{h} -- likely clamped by "
                    f"a dock/content minimum size (minimumSizeHint="
                    f"{win.minimumSizeHint().width()}x{win.minimumSizeHint().height()})",
                )
                central = win.centralWidget()
                self.assertIsNotNone(central)
                for dock in (win._left_dock, win._right_dock, win._bottom_dock):
                    self.assertLessEqual(dock.width(), win.width())
                    self.assertLessEqual(dock.height(), win.height())
            win.hide()
        finally:
            win.close()

    # MUTATION: re-verified after the fix-round-1 rework (Ignored ->
    # setMinimumWidth(1) on _sess/_seq/_disc; _link left unprotected by
    # any shrink treatment). Two prior candidate mutations were tried and
    # found NOT strong enough on their own (both honestly re-measured,
    # not assumed): (a) reverting the two dock `setMinimumWidth()` calls
    # (300->360, 280->320) alone -- min size stays under 1280 (headroom
    # from the content-level fixes). (b) reverting PreflightPanel's
    # `setWordWrap(True)` alone -- min size hint 1259 < 1280, still
    # passes. The real discriminator: in panels_info.py's
    # TopStatusStrip, changing `for _lbl in (self._sess, self._seq):` to
    # `for _lbl in ():` (dropping the `setMinimumWidth(1)` override, so
    # _sess/_seq fall back to their full-text-width floor and can never
    # compress) reliably fails this test at the 1280x720 iteration --
    # window resizes to 1318x720 instead (minimumSizeHint width 1318 >
    # 1280).

    # ── 9. LINK staleness readout stays visible under pressure ──
    def test_top_status_link_stays_visible_at_1280x720(self) -> None:
        """`_link` is safety-relevant (same tier as MODE/PHASE/HEALTH) and
        must never be clipped to zero width, even at the tightest
        supported window size. Regression test for a fix-round-1 bug
        where an `Ignored` size policy shared with a competing
        `addStretch()` drove `_link` to width 0 UNCONDITIONALLY, at every
        window size -- not just under pressure."""
        win = self._make_window(44210, 45210, "linkvisible")
        try:
            win.show()
            self._app.processEvents()
            win.resize(1280, 720)
            self._app.processEvents()
            top = win._top
            top._refresh_link()  # force a real staleness readout, not the ctor placeholder
            self.assertGreater(
                top._link.width(), 0,
                "the LINK staleness readout must stay visible (nonzero width) at 1280x720",
            )
            self.assertEqual(
                top._link.text(), "LINK: waiting",
                "no packet has been fed -- must show the real staleness readout, "
                "not the constructor's 'LINK: —' placeholder",
            )
            win.hide()
        finally:
            win.close()

    # MUTATION: re-apply the reverted `Ignored` policy to `_link`
    # (`self._link.setSizePolicy(QSizePolicy.Policy.Ignored, ...)`
    # alongside the others) and confirm test_top_status_link_stays_visible_at_1280x720
    # fails naming the "must stay visible (nonzero width)" assertion
    # (width becomes 0).

    # ── 10. sess/seq/disc are visible (not just non-clipped) when roomy ──
    def test_top_status_secondary_labels_visible_when_roomy(self) -> None:
        """_sess/_seq/_disc must render at their full natural width when
        there's plenty of room (1920x1080) -- they should only compress
        under genuine pressure, not be squashed unconditionally the way
        `Ignored` squashed them in fix-round-1."""
        win = self._make_window(44211, 45211, "secondaryvisible")
        try:
            win.show()
            self._app.processEvents()
            win.resize(1920, 1080)
            self._app.processEvents()
            top = win._top
            for lbl, name in ((top._sess, "sess"), (top._seq, "seq"), (top._disc, "disc")):
                self.assertGreater(
                    lbl.width(), 0, f"{name} label should be visible (nonzero width) at 1920x1080"
                )
            win.hide()
        finally:
            win.close()

    # MUTATION: change `for _lbl in (self._sess, self._seq):` to
    # `for _lbl in ():` in panels_info.py (drop the setMinimumWidth(1)
    # override) and confirm test_top_status_secondary_labels_visible_when_roomy
    # still passes for sess/seq (Preferred policy alone still renders
    # full width when roomy) -- then separately reapply `Ignored` to
    # `self._disc` and confirm the disc assertion fails naming "disc"
    # (width becomes 0 even at 1920x1080).


if __name__ == "__main__":
    unittest.main()
