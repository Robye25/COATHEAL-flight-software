"""The memorised bend cycle (app/bend_cycle.py): the BENDSEQ_LOAD line it
sends, its validation, the duration estimate, the BENDSEQ_STATUS progress
line and the settings round trip. No Qt."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import bend_cycle  # noqa: E402
from app.bend_cycle import BendCycle  # noqa: E402


class LoadCommandTests(unittest.TestCase):
    def test_limits_soaks_cycles_and_return(self) -> None:
        # 1 mm lead, 200 full steps/mm: 1.5 mm at µ4 is 1200 µsteps.
        cycle = BendCycle(name="flex", plus_mm=1.5, minus_mm=-0.5, cycles=3,
                          upper_soak_s=5.0, lower_soak_s=2.5, return_to_zero=True)
        self.assertEqual(bend_cycle.load_command(0, cycle, 4),
                         "BENDSEQ_LOAD 0 flex 1200:5 -400:2.5 repeat=3 0:0")
        # The live divisor, never a guess.
        self.assertEqual(bend_cycle.load_command(1, cycle, 16),
                         "BENDSEQ_LOAD 1 flex 4800:5 -1600:2.5 repeat=3 0:0")

    def test_single_cycle_has_no_repeat_and_a_zero_limit_no_tail(self) -> None:
        once = BendCycle(name="once", plus_mm=2.0, minus_mm=-2.0, cycles=1, upper_soak_s=10, lower_soak_s=0)
        self.assertEqual(bend_cycle.load_command(0, once, 4), "BENDSEQ_LOAD 0 once 1600:10 -1600:0 0:0")
        to_zero = BendCycle(name="pull", plus_mm=2.0, minus_mm=0.0, cycles=10, upper_soak_s=5, lower_soak_s=5)
        self.assertEqual(bend_cycle.load_command(0, to_zero, 4), "BENDSEQ_LOAD 0 pull 1600:5 0:5 repeat=10",
                         "the negative limit already is zero: no extra step")
        no_return = BendCycle(name="stay", plus_mm=1.0, minus_mm=-1.0, cycles=2, return_to_zero=False)
        self.assertEqual(bend_cycle.load_command(1, no_return, 4), "BENDSEQ_LOAD 1 stay 800:5 -800:5 repeat=2")

    def test_the_line_fits_the_request_budget(self) -> None:
        # The ground station refuses request lines above 230 bytes
        # (docs/link-budget.md); the widest cycle stays far below it.
        widest = BendCycle(name="x" * bend_cycle.MAX_NAME_LEN, plus_mm=500.0, minus_mm=-500.0,
                           cycles=bend_cycle.MAX_CYCLES, upper_soak_s=86400.0, lower_soak_s=86400.0)
        self.assertLess(len(bend_cycle.load_command(1, widest, 256).encode()), 120)


class ValidationTests(unittest.TestCase):
    def test_defaults_are_valid_and_each_rule_speaks(self) -> None:
        self.assertIsNone(bend_cycle.validate(BendCycle()))
        cases = [
            (BendCycle(name=""), "name"),
            (BendCycle(name="two words"), "name"),
            (BendCycle(name="y" * 33), "name"),
            (BendCycle(plus_mm=-1.0), "+ limit"),
            (BendCycle(minus_mm=0.5), "− limit"),
            (BendCycle(plus_mm=600.0), "+ limit"),
            (BendCycle(plus_mm=0.0, minus_mm=0.0), "equal"),
            (BendCycle(cycles=0), "cycles"),
            (BendCycle(cycles=1001), "cycles"),
            (BendCycle(upper_soak_s=-1.0), "soak at +"),
            (BendCycle(lower_soak_s=float("inf")), "soak at −"),
        ]
        for cycle, fragment in cases:
            self.assertIn(fragment, bend_cycle.validate(cycle) or "", cycle)


class DurationTests(unittest.TestCase):
    def test_travel_and_soaks_at_the_motor_speed(self) -> None:
        # From zero: 0→+2 (2 mm), +2→−2 (4 mm), two more cycles of 8 mm, back
        # to zero (2 mm) = 24 mm at 0.5 mm/s = 48 s, plus 3 × (5 + 5) s soak.
        cycle = BendCycle(plus_mm=2.0, minus_mm=-2.0, cycles=3, upper_soak_s=5, lower_soak_s=5)
        self.assertAlmostEqual(bend_cycle.duration_s(cycle, 0.5), 78.0)
        self.assertIsNone(bend_cycle.duration_s(cycle, None))
        self.assertIsNone(bend_cycle.duration_s(cycle, 0.0))
        text = bend_cycle.describe(cycle, 0.5)
        self.assertIn("3 × (+2.000 mm soak 5 s → -2.000 mm soak 5 s), then back to 0", text)
        self.assertIn("≈ 1 min 18 s at 0.50 mm/s", text)
        self.assertNotIn("≈", bend_cycle.describe(cycle, None), "no speed, no estimate")

    def test_duration_formats(self) -> None:
        self.assertEqual(bend_cycle.format_duration(45), "45 s")
        self.assertEqual(bend_cycle.format_duration(170), "2 min 50 s")
        self.assertEqual(bend_cycle.format_duration(3725), "1 h 02 min")


class ProgressTests(unittest.TestCase):
    def test_status_body_to_progress_line(self) -> None:
        self.assertEqual(bend_cycle.progress_text(
            "motor=0;zeroed=1;running=1;paused=0;name=cycle;step=4;total=21;cycle=3;cycles=10"),
            "cycle: cycle 3/10 · step 5/21 · running")
        self.assertEqual(bend_cycle.progress_text(
            "motor=0;zeroed=1;running=1;paused=1;name=flex;step=0;total=7;cycle=1;cycles=3;fault=motor disabled"),
            "flex: cycle 1/3 · step 1/7 · paused · fault: motor disabled")
        self.assertEqual(bend_cycle.progress_text("motor=0;zeroed=1;running=0;paused=0;name=;step=0"), "idle")
        self.assertIsNone(bend_cycle.progress_text("invalid motor"))
        self.assertIsNone(bend_cycle.progress_text("motor=0;running=1;name=x;step=a;total=b"))


class SettingsRoundTripTests(unittest.TestCase):
    def test_mapping_round_trip_and_string_values(self) -> None:
        cycle = BendCycle(name="flex", plus_mm=1.5, minus_mm=-0.5, cycles=3,
                          upper_soak_s=5.0, lower_soak_s=2.5, return_to_zero=False)
        self.assertEqual(bend_cycle.from_mapping(bend_cycle.to_mapping(cycle)), cycle)
        # QSettings on an .ini backend hands everything back as strings.
        as_strings = {k: str(v) for k, v in bend_cycle.to_mapping(cycle).items()}
        self.assertEqual(as_strings["return_to_zero"], "False")
        self.assertEqual(bend_cycle.from_mapping(as_strings), cycle)
        self.assertEqual(bend_cycle.from_mapping({"return_to_zero": "true"}).return_to_zero, True)

    def test_missing_or_broken_values_fall_back(self) -> None:
        default = BendCycle()
        self.assertEqual(bend_cycle.from_mapping({}), default)
        broken = bend_cycle.from_mapping({"plus_mm": "wide", "cycles": "5000", "name": "   ", "return_to_zero": "maybe"})
        self.assertEqual(broken.plus_mm, default.plus_mm)
        self.assertEqual(broken.cycles, default.cycles)
        self.assertEqual(broken.name, default.name)
        self.assertEqual(broken.return_to_zero, default.return_to_zero)
        self.assertEqual(bend_cycle.from_mapping({"cycles": "7.0", "minus_mm": "-1"}).cycles, 7)
        self.assertEqual(bend_cycle.FIELDS, ("name", "plus_mm", "minus_mm", "cycles", "upper_soak_s",
                                             "lower_soak_s", "return_to_zero"))


if __name__ == "__main__":
    unittest.main()
