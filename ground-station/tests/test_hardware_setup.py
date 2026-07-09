from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "hardware_setup.py"
SPEC = importlib.util.spec_from_file_location("hardware_setup", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
hardware_setup = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hardware_setup
SPEC.loader.exec_module(hardware_setup)


class HardwareSetupTests(unittest.TestCase):
    def test_modbus_crc_known_request(self) -> None:
        body = bytes.fromhex("010300000008")
        self.assertEqual(hardware_setup.modbus_crc(body), 0x0C44)

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
        self.assertTrue(any("BCM GPIO 17" in error for error in errors))

    def test_example_configuration_mappings_are_valid(self) -> None:
        source = hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")
        self.assertEqual(hardware_setup.validate_candidate(source), [])


if __name__ == "__main__":
    unittest.main()
