"""Thermal presets v2: atomic save with .bak, v1 migration, commands."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.thermal_presets import PresetStore, ThermalPreset  # noqa: E402


class PresetTests(unittest.TestCase):
    def test_commands_reproduce_the_preset(self) -> None:
        preset = ThermalPreset("ascent", targets_c=[8.0, 8.0, None, 8.0, None, None],
                               pid_all=(0.2, 0.02, 0.03), pid_channels={2: (0.3, 0.0, 0.0)})
        self.assertEqual(preset.commands(), [
            "SET_PID ALL 0.2 0.02 0.03", "SET_PID 2 0.3 0 0",
            "SET_TEMP_TARGET 0 8.000", "SET_TEMP_TARGET 1 8.000", "CLEAR_TEMP_TARGET 2",
            "SET_TEMP_TARGET 3 8.000", "CLEAR_TEMP_TARGET 4", "CLEAR_TEMP_TARGET 5",
        ])

    # MUTATION: emit SET_TEMP_TARGET before SET_PID in commands() and confirm
    # test_commands_reproduce_the_preset fails on ordering.

    def test_capture_from_get_thermal(self) -> None:
        body = ("target_min_c=0;target_max_c=80;h0_target=25;h0_temp=24.1;h0_duty=0.3;"
                "h1_target=-;h1_temp=-;h1_duty=0;h2_target=10.5;h2_temp=9;h2_duty=0.1;"
                "h3_target=-;h3_temp=1;h3_duty=0;h4_target=-;h4_temp=1;h4_duty=0;h5_target=-;h5_temp=1;h5_duty=0")
        preset = ThermalPreset.from_get_thermal("captured", body)
        self.assertEqual(preset.targets_c, [25.0, None, 10.5, None, None, None])
        self.assertTrue(preset.created_utc)

    def test_store_round_trip_atomic_and_backup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "profiles" / "thermal_presets.json"
            store = PresetStore(path).load()
            self.assertEqual(store.names(), [])
            store.put(ThermalPreset("a", targets_c=[1.0] + [None] * 5))
            self.assertTrue(path.exists())
            self.assertFalse(path.with_name("thermal_presets.json.bak").exists(), "no .bak before a second save")
            store.put(ThermalPreset("b"))
            self.assertTrue(path.with_name("thermal_presets.json.bak").exists())
            backup = json.loads(path.with_name("thermal_presets.json.bak").read_text(encoding="utf-8"))
            self.assertEqual(list(backup["presets"]), ["a"], ".bak holds the previous file")
            self.assertFalse(path.with_name("thermal_presets.json.tmp").exists())
            again = PresetStore(path).load()
            self.assertEqual(again.names(), ["a", "b"])
            self.assertEqual(again.get("a").targets_c[0], 1.0)
            self.assertTrue(again.rename("b", "c"))
            self.assertTrue(again.delete("a"))
            self.assertEqual(PresetStore(path).load().names(), ["c"])
            again.mark_applied("c", "2026-08-28T00:00:00Z")
            self.assertEqual(PresetStore(path).load().get("c").last_applied_utc, "2026-08-28T00:00:00Z")

    # MUTATION: remove the shutil.copyfile(...) line in PresetStore.save and
    # confirm test_store_round_trip_atomic_and_backup fails on the .bak
    # existence assertion.

    def test_v1_migration(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            legacy = Path(tmp) / "thermal_profiles.json"
            legacy.write_text(json.dumps({"version": 1, "profiles": {
                "cold": {"targets_c": [5, 5, 5, 5, 5, 5], "pid": {"kp": 0.5, "ki": 0.1, "kd": 0.0}}}}),
                encoding="utf-8")
            store = PresetStore(Path(tmp) / "thermal_presets.json").load()
            self.assertEqual(store.names(), ["cold"])
            self.assertEqual(store.get("cold").pid_all, (0.5, 0.1, 0.0))
            self.assertEqual(store.migrated_from, legacy)
            self.assertTrue(legacy.exists(), "the v1 file is never deleted")
            self.assertTrue((Path(tmp) / "thermal_presets.json").exists())

    def test_corrupt_file_is_reported_not_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "thermal_presets.json"
            path.write_text("{not json", encoding="utf-8")
            store = PresetStore(path).load()
            self.assertEqual(store.names(), [])
            self.assertIsNotNone(store.load_error)


if __name__ == "__main__":
    unittest.main()
