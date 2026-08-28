"""Headless GUI tests for the Health tab (HealthPanel) and the
TopStatusStrip aggregate health dot -- Task 2 of the GS UI/UX pass.

Follows the conventions of test_gui_smoke.py: offscreen QPA platform,
skip cleanly if PyQt6/Qt aren't usable, build a real MainWindow and push
scripted TelemetryPacket objects through `win._on_packet`.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# These pull in PyQt6 transitively (app.gui.panels_health imports
# PyQt6.QtWidgets at module level) -- guarded so an unavailable PyQt6
# fails cleanly via setUpClass's SkipTest below instead of crashing the
# whole module's import (and therefore the whole file's test discovery)
# with an uncaught ImportError, matching test_gui_smoke.py's convention
# of never importing app.gui.* at module level.
try:
    from app.protocol import TelemetryPacket
    from app.gui.panels_health import OK_FAIL_FLAGS, TRI_STATE_FLAGS, COMPONENTS
    from app.gui.panels_health import health_summary
except Exception as _import_exc:  # pragma: no cover - exercised only when PyQt6 is absent
    TelemetryPacket = None
    OK_FAIL_FLAGS, TRI_STATE_FLAGS, COMPONENTS = [], [], []
    health_summary = None
    _IMPORT_ERROR = _import_exc
else:
    _IMPORT_ERROR = None

GREEN = "#2ecc71"
RED = "#e74c3c"
AMBER = "#f39c12"
GRAY = "#666666"


def _packet(status: str = "", component_state=None, **overrides) -> TelemetryPacket:
    base = dict(
        session_id="sess-health-test", seq=1,
        timestamp="2026-08-19T00:00:00Z", rtc_valid=1,
        ambient_temp_c=-10.0, ambient_pressure_mbar=140.0, uv=0.1,
        sample_temps_c=[-5.0] * 8, heater_duty=[0.1] * 6,
        sample_resistance_ohm=[10.0] * 8, phase="FLOAT", mode="RUN",
        status=status, component_state=component_state or {},
    )
    base.update(overrides)
    return TelemetryPacket(**base)


ALL_OK_STATUS = "|".join(f"{key}_OK" for key, _ in OK_FAIL_FLAGS)
ALL_TRI_HEALTHY_STATUS = "|".join(green for _amber, green, _label in TRI_STATE_FLAGS)


class HealthPanelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _IMPORT_ERROR is not None:
            raise unittest.SkipTest(f"PyQt6 unavailable: {_IMPORT_ERROR}")
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
            cmd_host="127.0.0.1", log_path=Path(f"logs/health_{tag}.csv"),
            firewall_check=False,
        )

    # ── tab placement ──
    def test_health_tab_is_first_and_default_selected(self) -> None:
        from PyQt6.QtWidgets import QTabWidget

        win = self._make_window(44100, 45100, "tabpos")
        try:
            right_tabs = win._right_tabs
            self.assertIsNotNone(right_tabs, "could not locate the right-dock tab widget")
            self.assertEqual(right_tabs.tabText(0), "Health")
            self.assertEqual(right_tabs.currentIndex(), 0)
            self.assertIs(right_tabs.widget(0), win._health)
        finally:
            win.close()

    # ── all-OK packet -> every OK/FAIL dot green ──
    def test_all_ok_packet_makes_every_ok_fail_dot_green(self) -> None:
        win = self._make_window(44101, 45101, "allok")
        try:
            win._on_packet(_packet(status=ALL_OK_STATUS))
            for key, label in OK_FAIL_FLAGS:
                self.assertEqual(
                    win._health.dot_color(key), GREEN,
                    f"{key} ({label}) should be green when {key}_OK is present",
                )
        finally:
            win.close()

    # MUTATION: invert OK/FAIL detection in HealthPanel.on_packet to prove
    # this test discriminates. Documented here (not executed by CI) --
    # exercised manually during implementation per process requirements:
    #   swap `GREEN if ok else RED if fail else GRAY` to
    #   `RED if ok else GREEN if fail else GRAY`
    # and confirm test_all_ok_packet_makes_every_ok_fail_dot_green fails
    # with an AssertionError naming the flag ("SD (SD card) should be
    # green...").

    # ── one _FAIL -> that dot red AND aggregate red naming the flag ──
    def test_single_fail_flag_turns_only_that_dot_red_and_aggregate_names_it(self) -> None:
        win = self._make_window(44102, 45102, "onefail")
        try:
            status = ALL_OK_STATUS.replace("I2C_OK", "I2C_FAIL")
            win._on_packet(_packet(status=status))
            self.assertEqual(win._health.dot_color("I2C"), RED,
                              "I2C_FAIL present -> I2C dot must be red")
            for key, _label in OK_FAIL_FLAGS:
                if key == "I2C":
                    continue
                self.assertEqual(win._health.dot_color(key), GREEN,
                                  f"{key} must stay green when only I2C fails")
            self.assertEqual(win._top.health_color(), RED,
                              "aggregate dot must be red when any flag fails")
            self.assertIn("I2C", win._top.health_tooltip(),
                           "aggregate tooltip must name the offending flag")
        finally:
            win.close()

    def test_multiple_fail_flags_all_named_in_aggregate_tooltip(self) -> None:
        win = self._make_window(44103, 45103, "multifail")
        try:
            status = ALL_OK_STATUS.replace("I2C_OK", "I2C_FAIL").replace(
                "RESISTANCE_OK", "RESISTANCE_FAIL")
            win._on_packet(_packet(status=status))
            self.assertEqual(win._top.health_color(), RED)
            tooltip = win._top.health_tooltip()
            self.assertIn("I2C", tooltip)
            self.assertIn("RESISTANCE", tooltip)
        finally:
            win.close()

    # ── fix round 1: partial STATUS coverage must not paint green ──
    def test_partial_status_single_ok_token_is_gray_not_green(self) -> None:
        """A truncated STATUS field carrying exactly one `<key>_OK` token
        (13 of 14 flags entirely unreported) must NOT paint the aggregate
        dot green -- green requires all 14 OK/FAIL flags to be present and
        OK. The tooltip must name the unreported flags so an operator can
        tell 'partial telemetry' from 'everything checked out'."""
        win = self._make_window(44114, 45114, "partialok")
        try:
            win._on_packet(_packet(status="SD_OK"))
            self.assertEqual(win._top.health_color(), GRAY,
                              "one lone _OK token must not turn the aggregate green")
            tooltip = win._top.health_tooltip()
            self.assertIn("13", tooltip, "tooltip must report the unreported count")
            for key, _label in OK_FAIL_FLAGS:
                if key == "SD":
                    continue
                self.assertIn(key, tooltip,
                              f"{key} must be named among the unreported flags")
        finally:
            win.close()

    # MUTATION: revert `_health_summary` to the old any-OK rule (`if
    # any_ok: return GREEN, "All 14 system flags OK, components OK"`) and
    # confirm test_partial_status_single_ok_token_is_gray_not_green fails
    # by naming the wrong color ('#2ecc71' where gray was expected).

    def test_thirteen_ok_one_fail_is_red_naming_only_the_failer(self) -> None:
        """All 14 flags reported (13 as OK, 1 as FAIL) is the 'fully
        populated' case, distinct from the partial/unreported case above --
        it must stay red, naming only the one flag that actually failed."""
        win = self._make_window(44115, 45115, "thirteenok")
        try:
            status = ALL_OK_STATUS.replace("I2C_OK", "I2C_FAIL")
            win._on_packet(_packet(status=status))
            self.assertEqual(win._top.health_color(), RED)
            tooltip = win._top.health_tooltip()
            self.assertIn("I2C", tooltip)
            self.assertNotIn("unreported", tooltip,
                              "a fully-reported packet must never say 'unreported'")
        finally:
            win.close()

    def test_all_fourteen_ok_flags_aggregate_is_green(self) -> None:
        """Positive control for the fix above: full OK coverage must still
        reach green -- the partial-coverage fix must not have collaterally
        broken the true-positive path."""
        win = self._make_window(44116, 45116, "allfourteen")
        try:
            win._on_packet(_packet(status=ALL_OK_STATUS))
            self.assertEqual(win._top.health_color(), GREEN)
            self.assertEqual(win._top.health_tooltip(), "All 14 system flags OK, components OK")
        finally:
            win.close()

    def test_fully_empty_status_tooltip_says_no_flags_reported(self) -> None:
        """Fully-empty STATUS (all 14 flags unreported) is gray like the
        partial case, but gets its own short tooltip rather than an
        unwieldy 14-flag list."""
        win = self._make_window(44117, 45117, "fullyempty")
        try:
            win._on_packet(_packet(status=""))
            self.assertEqual(win._top.health_color(), GRAY)
            self.assertEqual(win._top.health_tooltip(), "no health flags reported")
        finally:
            win.close()

    # ── absent flags (legacy packet, no STATUS tokens) -> gray, aggregate gray ──
    def test_legacy_packet_with_no_status_tokens_is_gray_not_red(self) -> None:
        """Legacy replays with an empty/unrecognized STATUS field must stay
        visually quiet -- gray, never red -- for every OK/FAIL dot, every
        tri-state dot, and the aggregate."""
        win = self._make_window(44104, 45104, "legacy")
        try:
            win._on_packet(_packet(status=""))
            for key, _label in OK_FAIL_FLAGS:
                self.assertEqual(win._health.dot_color(key), GRAY,
                                  f"{key} must be gray with no STATUS tokens, not red/green")
            for amber_tok, _green_tok, _label in TRI_STATE_FLAGS:
                self.assertEqual(win._health.dot_color(amber_tok), GRAY,
                                  f"{amber_tok} must be gray with no STATUS tokens")
            self.assertEqual(win._top.health_color(), GRAY,
                              "aggregate must be gray when no known flags are present")
        finally:
            win.close()

    # MUTATION: change the legacy-gray branch to red (e.g.
    # `RED if not ok else ...`) and confirm
    # test_legacy_packet_with_no_status_tokens_is_gray_not_red fails,
    # naming the specific flag that turned unexpectedly red/green.

    def test_aggregate_dot_is_gray_before_any_packet(self) -> None:
        win = self._make_window(44105, 45105, "beforepacket")
        try:
            self.assertEqual(win._top.health_color(), GRAY,
                              "aggregate dot must be gray before the first packet")
        finally:
            win.close()

    # ── tri-state amber cases ──
    def test_tri_state_flags_amber_when_amber_token_present(self) -> None:
        win = self._make_window(44106, 45106, "triamber")
        try:
            status = "|".join(amber for amber, _green, _label in TRI_STATE_FLAGS)
            win._on_packet(_packet(status=status))
            for amber_tok, _green_tok, label in TRI_STATE_FLAGS:
                self.assertEqual(
                    win._health.dot_color(amber_tok), AMBER,
                    f"{amber_tok} ({label}) should be amber when its own token is present",
                )
        finally:
            win.close()

    def test_tri_state_flags_green_when_green_token_present(self) -> None:
        win = self._make_window(44107, 45107, "trigreen")
        try:
            win._on_packet(_packet(status=ALL_TRI_HEALTHY_STATUS))
            for amber_tok, _green_tok, label in TRI_STATE_FLAGS:
                self.assertEqual(
                    win._health.dot_color(amber_tok), GREEN,
                    f"{amber_tok} ({label}) should be green when the healthy token is present",
                )
        finally:
            win.close()

    # MUTATION: swap the amber/green branch bodies in HealthPanel.on_packet
    # (`color = GREEN if amber_tok in tokens else AMBER if green_tok in
    # tokens else GRAY`) and confirm both
    # test_tri_state_flags_amber_when_amber_token_present and
    # test_tri_state_flags_green_when_green_token_present fail, each
    # naming the tri-state flag whose color inverted.

    # ── COMPONENT_STATE mapping ──
    def test_component_state_ok_is_green(self) -> None:
        win = self._make_window(44108, 45108, "compok")
        try:
            states = {key: "OK" for key, _label in COMPONENTS}
            win._on_packet(_packet(component_state=states))
            for key, _label in COMPONENTS:
                self.assertEqual(win._health.dot_color(f"component_{key}"), GREEN)
                self.assertEqual(win._health.state_word(f"component_{key}"), "OK")
        finally:
            win.close()

    def test_component_state_degraded_stale_failed_are_red(self) -> None:
        """Rule: DEGRADED, STALE, and FAILED (the three explicitly
        degraded/error component states in
        onboard/include/coatheal/component_health.hpp) map to red."""
        win = self._make_window(44109, 45109, "compred")
        try:
            states = {
                "DPS310": "DEGRADED", "ADS1115": "STALE", "SEQUENT_RTD": "FAILED",
                "MOTOR0": "OK", "MOTOR1": "OK", "PWM": "OK",
            }
            win._on_packet(_packet(component_state=states))
            self.assertEqual(win._health.dot_color("component_DPS310"), RED)
            self.assertEqual(win._health.dot_color("component_ADS1115"), RED)
            self.assertEqual(win._health.dot_color("component_SEQUENT_RTD"), RED)
            self.assertEqual(win._health.state_word("component_DPS310"), "DEGRADED")
            self.assertEqual(win._health.state_word("component_ADS1115"), "STALE")
            self.assertEqual(win._health.state_word("component_SEQUENT_RTD"), "FAILED")
        finally:
            win.close()

    def test_component_state_discovering_is_gray_not_red(self) -> None:
        """Rule: DISCOVERING (boot-time, not yet resolved) is gray, not
        red -- it isn't an error, it just hasn't resolved yet."""
        win = self._make_window(44110, 45110, "compgray")
        try:
            states = {
                "DPS310": "DISCOVERING",
                "ADS1115": "OK", "SEQUENT_RTD": "OK", "MOTOR0": "OK",
                "MOTOR1": "OK", "PWM": "OK",
            }
            win._on_packet(_packet(component_state=states))
            self.assertEqual(win._health.dot_color("component_DPS310"), GRAY)
        finally:
            win.close()

    def test_component_state_disabled_is_amber_not_gray(self) -> None:
        """Design ruling (fix round 1): DISABLED must be amber, not gray --
        sharing gray with 'unknown/absent' made a deliberately-disabled
        component (e.g. off by config mistake) look identical to 'not
        reported'. Amber already means 'non-nominal, not failing' for the
        tri-state flags; a disabled component fits that meaning."""
        win = self._make_window(44118, 45118, "compamber")
        try:
            states = {
                "ADS1115": "DISABLED",
                "DPS310": "OK", "SEQUENT_RTD": "OK", "MOTOR0": "OK",
                "MOTOR1": "OK", "PWM": "OK",
            }
            win._on_packet(_packet(component_state=states))
            self.assertEqual(win._health.dot_color("component_ADS1115"), AMBER,
                              "DISABLED must render amber, not gray")
            self.assertEqual(win._health.state_word("component_ADS1115"), "DISABLED")
        finally:
            win.close()

    # MUTATION: move DISABLED back into the gray fallback (drop it from
    # `_COMPONENT_AMBER`) and confirm test_component_state_disabled_is_amber_not_gray
    # fails, naming the DISABLED-must-render-amber assertion.

    def test_component_state_unknown_word_maps_to_gray_not_red(self) -> None:
        """Rule: a state word this GUI doesn't recognize (future firmware,
        typo, anything not in {OK, DEGRADED, STALE, FAILED}) must render
        gray, not red -- the same 'never panic on what we don't understand'
        rule the OK/FAIL flags use for legacy replays. A red default here
        would misrepresent an unrecognized-but-possibly-fine state as a
        confirmed failure."""
        win = self._make_window(44111, 45111, "compunknown")
        try:
            states = {"DPS310": "TOTALLY_MADE_UP_STATE"}
            win._on_packet(_packet(component_state=states))
            self.assertEqual(win._health.dot_color("component_DPS310"), GRAY)
            self.assertEqual(win._health.state_word("component_DPS310"),
                              "TOTALLY_MADE_UP_STATE")
        finally:
            win.close()

    def test_component_state_absent_key_is_gray_with_em_dash(self) -> None:
        win = self._make_window(44112, 45112, "compabsent")
        try:
            win._on_packet(_packet(component_state={}))
            for key, _label in COMPONENTS:
                self.assertEqual(win._health.dot_color(f"component_{key}"), GRAY)
                self.assertEqual(win._health.state_word(f"component_{key}"), "—")
        finally:
            win.close()

    # MUTATION: change `component_color()`'s red set from
    # {"DEGRADED", "STALE", "FAILED"} to {"DEGRADED", "STALE"} (dropping
    # FAILED) and confirm test_component_state_degraded_stale_failed_are_red
    # fails on the SEQUENT_RTD/FAILED assertion, naming the missed state.

    # ── ValuesPanel dedup ──
    def test_valuespanel_no_longer_shows_removed_flag_text_rows(self) -> None:
        """The STATUS section's flag rows (raw bitfield, heater inhibit,
        resistance OK/FAIL, and the 6 COMPONENT_STATE rows) must be gone
        from ValuesPanel -- the Health tab is their only home now."""
        win = self._make_window(44113, 45113, "dedup")
        try:
            self.assertNotIn("status", win._values._fields)
            self.assertNotIn("heater_inhibit", win._values._fields)
            self.assertNotIn("resistance_ok", win._values._fields)
            for component in ("DPS310", "ADS1115", "SEQUENT_RTD", "MOTOR0", "MOTOR1", "PWM"):
                self.assertNotIn(f"component_{component}", win._values._fields)
            # Numeric/value rows must survive the dedup.
            for surviving in ("ambient_temp_c", "sample_0", "resistance_0", "m0_state"):
                self.assertIn(surviving, win._values._fields)
        finally:
            win.close()

    # MUTATION: re-add `self._row("heater_inhibit", "heater inhibit")` to
    # ValuesPanel.__init__ and confirm
    # test_valuespanel_no_longer_shows_removed_flag_text_rows fails on the
    # heater_inhibit assertNotIn line.

    # ── fix round: aggregate must not go green over a red component or
    # simulated sensors (Task 3 fix round 2) ──
    def test_component_failed_makes_aggregate_red_even_with_all_ok_flags(self) -> None:
        """Proven contradiction: all 14 OK/FAIL flags OK plus
        COMPONENT_STATE=MOTOR0:FAILED|DPS310:FAILED previously painted the
        aggregate green ("All health flags OK") while the Health tab
        showed two red FAILED dots. The aggregate must go red and name
        the failed components.

        Tests `panels_health.health_summary` directly (a pure function,
        no widget construction needed) rather than through a full
        MainWindow -- the on_packet()->_health_summary() wiring itself is
        already exercised by every other test in this file; a MainWindow
        per test here just adds unnecessary pyqtgraph/Qt object churn to
        the full suite run for no additional coverage."""
        color, tooltip = health_summary(_packet(
            status=ALL_OK_STATUS,
            component_state={"MOTOR0": "FAILED", "DPS310": "FAILED"},
        ))
        self.assertEqual(color, RED,
                          "a red component must override an all-OK flag set")
        self.assertIn("MOTOR0", tooltip)
        self.assertIn("FAILED", tooltip)
        self.assertIn("DPS310", tooltip)

    # MUTATION: drop the `or red_components` clause from the `if failing
    # or red_components:` check in panels_health.py's `health_summary` and
    # confirm test_component_failed_makes_aggregate_red_even_with_all_ok_flags
    # fails, reporting green instead of red.

    def test_simulated_sensors_make_aggregate_amber_not_green(self) -> None:
        """Proven contradiction: all 14 OK/FAIL flags OK while SIMULATED
        is active (fake sensor data) previously painted the aggregate a
        plain green all-clear, hiding that the data isn't real. Must be
        amber -- not green (hides the fakery) and not red (SIMULATED
        isn't a failure). Same no-MainWindow rationale as the test above."""
        color, tooltip = health_summary(
            _packet(status=ALL_OK_STATUS + "|SIMULATED")
        )
        self.assertEqual(color, AMBER,
                          "SIMULATED must be amber, not a plain green all-clear")
        self.assertEqual(tooltip, "running on simulated sensors")

    # MUTATION: delete the `if "SIMULATED" in tokens: return AMBER, ...`
    # branch from `health_summary` in panels_health.py and confirm
    # test_simulated_sensors_make_aggregate_amber_not_green fails,
    # reporting green instead of amber.


if __name__ == "__main__":
    unittest.main()
