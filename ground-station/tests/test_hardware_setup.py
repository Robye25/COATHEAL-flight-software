from __future__ import annotations

import importlib.util
import argparse
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "hardware_setup.py"
SPEC = importlib.util.spec_from_file_location("hardware_setup", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
hardware_setup = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hardware_setup
SPEC.loader.exec_module(hardware_setup)


def migrated_values(source: Path) -> dict[str, str]:
    """Return the ini key/value mapping migrate_config would write for
    `source`, without touching disk.

    `_candidate_from_existing` already applies the RETIRED_SENSOR_KEYS /
    OBSOLETE_CONFIG_KEYS filtering and FINAL_PIN_VALUES overrides and
    returns the merged config as text; `_ini_values` (the same parser
    `_load_config` wraps around a file read) turns that text into a dict.
    `_load_config` itself is not reusable here since it takes a Path and
    reads from disk, and this helper is deliberately disk-free.
    """
    text = hardware_setup._candidate_from_existing(source)
    return hardware_setup._ini_values(text)


class HardwareSetupTests(unittest.TestCase):
    def test_replace_ini_replaces_and_appends(self) -> None:
        result = hardware_setup.replace_ini(
            "a=1\n# retained\n", {"a": "2", "b": "3"})
        self.assertIn("a=2\n", result)
        self.assertIn("# retained\n", result)
        self.assertTrue(result.endswith("b=3\n"))

    def test_validate_candidate_detects_gpio_conflict(self) -> None:
        # motor0.enable_line collides with heater.output_lines[0] (BCM 19,
        # see FINAL_PIN_VALUES/EXAMPLE_CONFIG) - a plain same-chip collision
        # between two non-reserved owners, distinct from the reserved-line
        # collision covered by test_validate_candidate_detects_reserved_gpio_collision.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"motor0.enable_line": "19"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertTrue(
            any("/dev/gpiochip0 line 19" in error for error in errors))

    def test_validate_candidate_detects_reserved_gpio_collision(self) -> None:
        # v3 reserved lines (Sequent RTD HAT + hardware SPI0 chip-selects)
        # must never be claimable by a heater. Six-entry list (matches
        # hardware.heater_count=6) so the count check passes and the GPIO
        # claim check is actually reached.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"heater.output_lines": "17,13,6,5,24,23"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertTrue(
            any("reserved: sequent_hat" in error for error in errors))

    def test_validate_candidate_rejects_retired_tmc2240_driver(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"motor0.driver": "tmc2240"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "motor0.driver=tmc2240 is retired; use tmc5160", errors)

    def test_validate_candidate_rejects_bad_sense_resistor(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"motor0.sense_resistor_ohm": "0"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "motor0.sense_resistor_ohm must be in (0, 1)", errors)

    def test_validate_candidate_detects_sequent_rtd_stack_out_of_range(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_stack": "8"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn("sensor.sequent_rtd_stack must be 0..7", errors)

    def test_validate_candidate_detects_duplicate_sequent_rtd_channel(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_channels": "1,2,3,4,5,6,7,7"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_channels contains duplicates", errors)

    def test_validate_candidate_channel_count_tracks_sample_count(self) -> None:
        # config.cpp compares sequent_rtd_channels.size() against the
        # *variable* hardware.sample_count, not a literal 8. Shrink
        # sample_count to 4 (heater.output_lines/temperature_channels must
        # shrink to match hardware.heater_count too, or an unrelated
        # required-mapping error fires first) and confirm the still-8-long
        # channel list is now rejected against the new count of 4, not 8.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source,
            {
                "hardware.sample_count": "4",
                "hardware.heater_count": "4",
                "heater.output_lines": "17,18,27,5",
                "heater.temperature_channels": "0,1,2,3",
            },
        )
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_channels must list hardware.sample_count "
            "entries", errors)

    def test_validate_candidate_detects_sequent_rtd_channel_out_of_range(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_channels": "1,2,3,4,5,6,7,9"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_channels entries must be 1..8", errors)

    SENSOR_TYPE_ERROR = (
        "sensor.sequent_rtd_expect_sensor_type must be pt100 "
        "(pt1000 is recognised but not implemented: the CVD "
        "cross-check and resistance window are PT100-only)")

    def test_validate_candidate_detects_bad_sequent_rtd_sensor_type(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_expect_sensor_type": "pt500"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(self.SENSOR_TYPE_ERROR, errors)

    def test_validate_candidate_rejects_pt1000_sensor_type(self) -> None:
        # Recorded spec deviation: pt1000 is advertised on the card and
        # handled by Probe(), but ApplyValidation's cross-check is PT100-only
        # (hardcoded CVD curve, 60-390 ohm window), so a pt1000 config would
        # invalidate every channel forever. config.cpp rejects it at load and
        # this validator must agree, or `hardware_setup` would bless a config
        # the onboard service then refuses to start on.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_expect_sensor_type": "pt1000"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(self.SENSOR_TYPE_ERROR, errors)

    def test_validate_candidate_detects_inverted_resistance_window(self) -> None:
        # Mirrors config.cpp:680-687. Exact-string assertion: the fragment is
        # unique across every error this validator can emit, so deleting the
        # rule fails this test rather than passing on a neighbouring message.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source,
            {
                "sensor.sequent_rtd_resistance_min_ohm": "390.0",
                "sensor.sequent_rtd_resistance_max_ohm": "60.0",
            },
        )
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_resistance_min_ohm must be below "
            "sensor.sequent_rtd_resistance_max_ohm", errors)

    def test_validate_candidate_rejects_equal_resistance_bounds(self) -> None:
        # config.cpp uses >=, not >: an empty window is as unusable as an
        # inverted one, and this is the case that separates the two operators.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source,
            {
                "sensor.sequent_rtd_resistance_min_ohm": "100.0",
                "sensor.sequent_rtd_resistance_max_ohm": "100.0",
            },
        )
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_resistance_min_ohm must be below "
            "sensor.sequent_rtd_resistance_max_ohm", errors)

    def test_validate_candidate_detects_bad_resistance_source(self) -> None:
        # Mirrors config.cpp:619-626, including the two legacy labels that
        # stay accepted so a fielded INI still loads.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.resistance_source": "ina3221"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.resistance_source must be disabled, simulated, "
            "or sequent_rtd", errors)

    def test_validate_candidate_accepts_every_resistance_source(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        for value in ("sequent_rtd", "disabled", "simulated"):
            candidate = hardware_setup.replace_ini(
                source, {"sensor.resistance_source": value})
            self.assertEqual(
                hardware_setup.validate_candidate(candidate), [], value)

    def test_same_line_on_different_gpio_chips_is_valid(self) -> None:
        # BCM 17 is a v3-reserved line (Sequent HAT rs485_dir) on the default
        # gpio_chip, but motor0 is moved to a *different* chip here, so the
        # reserved claim (scoped to runtime.gpio_chip) must not collide.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        candidate = hardware_setup.replace_ini(
            source,
            {
                "motor0.gpio_chip": "/dev/gpiochip1",
                "motor0.cs_line": "17",
            },
        )
        self.assertEqual(hardware_setup.validate_candidate(candidate), [])

    def test_motor_current_rejects_sense_resistor_ceiling(self) -> None:
        # TMC5160 hardware ceiling: at the default motor0.sense_resistor_ohm
        # (0.075, see EXAMPLE_CONFIG), the sense resistor's maximum
        # deliverable current is 0.325/0.075 = 4.3333 A_peak, i.e.
        # 4.3333/sqrt(2) = 3.0641 A_rms. 3.08 A_rms sits just above that
        # (3.08*sqrt(2) = 4.3558 A_peak > 4.3333) while staying under the
        # *separate* flat (0, 3.1] ceiling -- deliberately chosen so this
        # test isolates the sense-resistor-derived rule: deleting only that
        # rule (leaving the flat bound in place) must make this assertion
        # fail, since 3.08 alone would then pass validation.
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        candidate = hardware_setup.replace_ini(
            source, {"motor0.run_current_a_rms": "3.08"})
        errors = hardware_setup.validate_candidate(candidate)
        self.assertTrue(any(
            "exceeds the sense resistor's deliverable current ceiling"
            in error for error in errors))

    def test_example_configuration_mappings_are_valid(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        self.assertEqual(hardware_setup.validate_candidate(source), [])

    def test_migrate_config_removes_stale_keys_and_forces_rtd_tmc5160(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old_path = root / "onboard.ini"
            new_path = root / "onboard.local.ini"
            old_text = hardware_setup.replace_ini(
                hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8"),
                {
                    # Retired driver identity: migration must force tmc5160
                    # regardless of what a stale source config says.
                    "motor0.driver": "tmc2240",
                    "motor1.driver": "tmc2240",
                    "sensor.sample_temperature_source": "daq132m_modbus",
                    "sensor.daq132m_enabled": "true",
                    "sensor.rtd_click_enabled": "false",
                },
            )
            old_text += (
                "stepper.microstep=16\nmotor0.sense_resistor=0.075\n"
                "motor0.step_line=19\nmotor0.dir_line=26\n"
                "motor0.pulse_high_us=3\n"
            )
            old_path.write_text(old_text, encoding="utf-8")

            rc = hardware_setup.migrate_config(argparse.Namespace(
                config=new_path,
                migrate_from=old_path,
                yes=True,
            ))
            self.assertEqual(rc, 0)
            migrated = new_path.read_text(encoding="utf-8")
            values = hardware_setup._ini_values(migrated)
            self.assertEqual(values["motor0.driver"], "tmc5160")
            self.assertEqual(values["motor1.driver"], "tmc5160")
            for retired in (
                "sensor.sample_temperature_source",
                "sensor.daq132m_enabled",
                "sensor.rtd_click_enabled",
            ):
                self.assertNotIn(retired, values)
            self.assertEqual(values["sensor.sequent_rtd_stack"], "0")
            self.assertEqual(
                values["sensor.sequent_rtd_channels"], "1,2,3,4,5,6,7,8")
            self.assertNotIn("stepper.microstep=", migrated)
            self.assertNotIn("motor0.sense_resistor=", migrated)
            self.assertNotIn("motor0.step_line=", migrated)
            self.assertNotIn("motor0.dir_line=", migrated)
            self.assertNotIn("motor0.pulse_high_us=", migrated)
            self.assertTrue(list(root.glob("onboard.ini.bak.*")))
            if os.name != "nt":
                self.assertEqual(new_path.stat().st_mode & 0o777, 0o644)

    def test_migrate_config_drops_retired_sensor_keys(self) -> None:
        # This confirms the behavioural property (retired keys never survive
        # into a migrated config) through whichever mechanism produces it:
        # today that is the union of the `key in template_keys` guard (none
        # of these keys are in EXAMPLE_CONFIG any more) and the explicit
        # RETIRED_SENSOR_KEYS blocklist. It does not isolate which
        # mechanism did the work — see
        # test_retired_keys_dropped_even_if_template_regresses below for a
        # test that fails specifically when RETIRED_SENSOR_KEYS is removed.
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "old.ini"
            source.write_text(
                "sensor.sample_temperature_source=rtd_click_max31865\n"
                "sensor.rtd_click_enabled=true\n"
                "sensor.rtd_click_drdy_line=25\n"
                "sensor.daq132m_enabled=false\n"
                "sensor.daq132m_device=/dev/ttyUSB0\n",
                encoding="utf-8",
            )
            values = migrated_values(source)
            for retired in (
                "sensor.sample_temperature_source",
                "sensor.rtd_click_enabled",
                "sensor.rtd_click_drdy_line",
                "sensor.daq132m_enabled",
                "sensor.daq132m_device",
            ):
                self.assertNotIn(retired, values)
            self.assertEqual(values["sensor.sequent_rtd_stack"], "0")
            self.assertEqual(
                values["sensor.sequent_rtd_channels"], "1,2,3,4,5,6,7,8")

    def test_migrate_config_drops_retired_motor_keys(self) -> None:
        # Same pattern as test_migrate_config_drops_retired_sensor_keys:
        # step_line/dir_line/pulse_high_us no longer exist (v3 has no
        # STEP/DIR lines) and must never survive migration, through the
        # OBSOLETE_CONFIG_KEYS blocklist extended for them. current_range_a_
        # peak (the retired TMC2240 range-select current model) gets the
        # same treatment.
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "old.ini"
            source.write_text(
                "motor0.step_line=19\n"
                "motor0.dir_line=26\n"
                "motor0.pulse_high_us=3\n"
                "motor0.current_range_a_peak=0\n"
                "motor1.step_line=24\n"
                "motor1.dir_line=20\n"
                "motor1.pulse_high_us=3\n"
                "motor1.current_range_a_peak=0\n",
                encoding="utf-8",
            )
            values = migrated_values(source)
            for retired in (
                "motor0.step_line", "motor0.dir_line", "motor0.pulse_high_us",
                "motor0.current_range_a_peak",
                "motor1.step_line", "motor1.dir_line", "motor1.pulse_high_us",
                "motor1.current_range_a_peak",
            ):
                self.assertNotIn(retired, values)
            self.assertEqual(values["motor0.driver"], "tmc5160")
            self.assertEqual(values["motor0.sense_resistor_ohm"], "0.075")

    def test_retired_keys_dropped_even_if_template_regresses(self) -> None:
        # RETIRED_SENSOR_KEYS is deliberately redundant with the
        # template-key guard today. This test disables the redundancy by
        # patching a retired key into the template with one value, then
        # feeding a *different* value for that same key through the
        # existing/source config being migrated. `_candidate_from_existing`
        # always starts from the template text and only overwrites keys
        # that make it into `updates` (see replace_ini): with the
        # RETIRED_SENSOR_KEYS filter removed, `key in template_keys` alone
        # would let the source's stale value into `updates`, and it would
        # overwrite the template's line. With the filter, the source's
        # value never reaches `updates`, so the template's own value is
        # what survives untouched. Deleting the RETIRED_SENSOR_KEYS clause
        # makes this fail (asserts "false", gets "true").
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "old.ini"
            source.write_text(
                "sensor.rtd_click_enabled=true\n", encoding="utf-8")
            template = hardware_setup.EXAMPLE_CONFIG.read_text(
                encoding="utf-8")
            patched = template + "\nsensor.rtd_click_enabled=false\n"
            fake_template = Path(tmp) / "example.ini"
            fake_template.write_text(patched, encoding="utf-8")
            with mock.patch.object(
                    hardware_setup, "EXAMPLE_CONFIG", fake_template):
                values = migrated_values(source)
            self.assertEqual(values["sensor.rtd_click_enabled"], "false")

    def test_sequent_rtd_reply_ok_accepts_both_token_casings(self) -> None:
        # Real hardware: SensorManager::ActiveCheck formats the reply and
        # emits lowercase `sequent_rtd=OK`.
        self.assertTrue(hardware_setup.sequent_rtd_reply_ok(
            "overall=OK;selected=SEQUENT_RTD;storage=SKIPPED;dps310=SKIPPED;"
            "ads1115=SKIPPED;sequent_rtd=OK;sequent_rtd_error=NONE;"
            "sequent_rtd_addr=0x40;sequent_rtd_burst=1;pwm=OK"))
        # Simulated build: ActiveCheck short-circuits and echoes the
        # requested component name verbatim, so the token is uppercase.
        self.assertTrue(hardware_setup.sequent_rtd_reply_ok(
            "overall=OK;selected=SEQUENT_RTD;SEQUENT_RTD=OK;simulated=1"))

    def test_sequent_rtd_reply_ok_rejects_failures(self) -> None:
        self.assertFalse(hardware_setup.sequent_rtd_reply_ok(
            "overall=FAIL;sequent_rtd=FAIL;sequent_rtd_error=NO_RESPONSE"))
        # overall=OK alone must not pass: the card token has to be present.
        self.assertFalse(hardware_setup.sequent_rtd_reply_ok(
            "overall=OK;selected=PWM;pwm=OK"))
        # Neighbouring keys that merely start with the same prefix must not
        # satisfy the whole-token match.
        self.assertFalse(hardware_setup.sequent_rtd_reply_ok(
            "overall=OK;sequent_rtd=FAIL;sequent_rtd_error=OK"))


if __name__ == "__main__":
    unittest.main()
