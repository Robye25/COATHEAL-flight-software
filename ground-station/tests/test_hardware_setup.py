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
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"motor0.step_line": "17"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertTrue(
            any("/dev/gpiochip0 line 17" in error for error in errors))

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

    def test_validate_candidate_detects_bad_sequent_rtd_sensor_type(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        broken = hardware_setup.replace_ini(
            source, {"sensor.sequent_rtd_expect_sensor_type": "pt500"})
        errors = hardware_setup.validate_candidate(broken)
        self.assertIn(
            "sensor.sequent_rtd_expect_sensor_type must be pt100 or pt1000",
            errors)

    def test_same_line_on_different_gpio_chips_is_valid(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        candidate = hardware_setup.replace_ini(
            source,
            {
                "motor0.gpio_chip": "/dev/gpiochip1",
                "motor0.step_line": "17",
            },
        )
        self.assertEqual(hardware_setup.validate_candidate(candidate), [])

    def test_tmc2240_current_must_fit_selected_range(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        candidate = hardware_setup.replace_ini(
            source,
            {
                "motor0.run_current_a_rms": "0.8",
                "motor0.current_range_a_peak": "1",
            },
        )
        errors = hardware_setup.validate_candidate(candidate)
        self.assertTrue(any("does not fit selected peak range" in error
                            for error in errors))

    def test_tmc2240_rejects_unusable_global_scaler(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        candidate = hardware_setup.replace_ini(
            source, {"motor0.run_current_a_rms": "0.01"})
        errors = hardware_setup.validate_candidate(candidate)
        self.assertTrue(any("invalid GLOBALSCALER" in error
                            for error in errors))

    def test_example_configuration_mappings_are_valid(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        self.assertEqual(hardware_setup.validate_candidate(source), [])

    def test_migrate_config_removes_stale_keys_and_forces_rtd_tmc2240(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old_path = root / "onboard.ini"
            new_path = root / "onboard.local.ini"
            old_text = hardware_setup.replace_ini(
                hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8"),
                {
                    "motor0.driver": "tmc5160",
                    "motor1.driver": "tmc5160",
                    "sensor.sample_temperature_source": "daq132m_modbus",
                    "sensor.daq132m_enabled": "true",
                    "sensor.rtd_click_enabled": "false",
                },
            )
            old_text += "stepper.microstep=16\nmotor0.sense_resistor=0.075\n"
            old_path.write_text(old_text, encoding="utf-8")

            rc = hardware_setup.migrate_config(argparse.Namespace(
                config=new_path,
                migrate_from=old_path,
                yes=True,
            ))
            self.assertEqual(rc, 0)
            migrated = new_path.read_text(encoding="utf-8")
            values = hardware_setup._ini_values(migrated)
            self.assertEqual(values["motor0.driver"], "tmc2240")
            self.assertEqual(values["motor1.driver"], "tmc2240")
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


if __name__ == "__main__":
    unittest.main()
