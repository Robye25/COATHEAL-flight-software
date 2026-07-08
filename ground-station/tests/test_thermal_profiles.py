import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.thermal_profiles import build_profile, load_profiles, save_profiles


class ThermalProfileTests(unittest.TestCase):
    def test_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profiles.json"
            profiles = {
                "bench": build_profile(
                    [20.0, 21.0, 22.0, 23.0, 24.0, 25.0],
                    0.2,
                    0.02,
                    0.03,
                )
            }
            save_profiles(profiles, path)
            self.assertEqual(load_profiles(path), profiles)

    def test_missing_or_invalid_file_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profiles.json"
            self.assertEqual(load_profiles(path), {})
            path.write_text("not-json", encoding="utf-8")
            self.assertEqual(load_profiles(path), {})


if __name__ == "__main__":
    unittest.main()
