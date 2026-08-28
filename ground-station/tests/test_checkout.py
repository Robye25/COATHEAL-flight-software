"""Go/no-go checklist derivation (redesign spec §5.7)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.checkout import checkout_items  # noqa: E402
from app.gui.state import OnboardState, state_from_packet  # noqa: E402
from app.protocol import parse_telemetry_csv  # noqa: E402

GOOD = ("DATA,coatheal-1-1,1,2026-08-28T00:00:00Z,1,20,1000,0.1,1,2,3,4,5,6,7,8,"
        "HEATER_DUTY=0|0|0|0|0|0,RESISTANCE=118|-|-|-|121|-|-|-,PHASE=ASCENT,MODE=RUN,"
        "STATUS=SD_OK|USB_OK|LINK_OK|ENERGY_OK|RESISTANCE_OK,"
        "COMPONENT_STATE=DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK,"
        "CTRL=fallback:0|energy_wh:12.4|budget_wh:130.0|budget_exhausted:0|queue:0,"
        "STEPPER0=pos:0|tgt:0|hz:100|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0|pulses:0|src:init|zeroed:1,"
        "STEPPER1=pos:0|tgt:0|hz:100|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0|pulses:0|src:init|zeroed:0")


def items(line=GOOD, link_age=0.5, **kw):
    st = state_from_packet(parse_telemetry_csv(line), link_age_s=link_age)
    return {i.key: i for i in checkout_items(st, link_ok=True, **kw)}


class CheckoutTests(unittest.TestCase):
    def test_all_green_scene(self) -> None:
        it = items()
        greens = [k for k, i in it.items() if i.color == "green"]
        self.assertEqual(it["link"].color, "green")
        self.assertEqual(it["pt100"].note, "8/8")
        self.assertEqual(it["max31865"].color, "green")
        self.assertEqual(it["m0"].color, "green")
        self.assertEqual(it["m1"].color, "amber")
        self.assertEqual(it["m1"].note, "not zeroed")
        self.assertEqual(it["energy"].note, "12.4 / 130 Wh")
        self.assertEqual(it["mode"].color, "green")
        self.assertEqual(it["alarms"].color, "green")
        self.assertGreaterEqual(len(greens), 9)

    def test_degradations(self) -> None:
        line = GOOD.replace("MODE=RUN", "MODE=STANDBY").replace("1,2,3,4,5,6,7,8,", "1,nan,3,4,5,6,7,8,") \
                   .replace("MOTOR0:OK", "MOTOR0:FAILED").replace("ok:1|en:1|mv:0|hold:0|hold_s:0|pulses:0|src:init|zeroed:1", "ok:0|en:0|mv:0|hold:0|hold_s:0|pulses:0|src:init|zeroed:0") \
                   .replace("SD_OK|USB_OK", "SD_OK|USB_FAIL")
        it = items(line, link_age=6.0, unacked_alarms=2)
        self.assertEqual(it["link"].color, "red")
        self.assertEqual(it["pt100"].color, "amber")
        self.assertEqual(it["pt100"].note, "7/8")
        self.assertEqual(it["m0"].color, "red")
        self.assertEqual(it["storage"].color, "red")
        self.assertEqual(it["storage"].note, "USB_FAIL")
        self.assertEqual(it["mode"].color, "amber")
        self.assertEqual(it["alarms"].color, "red")

    # MUTATION: make the pt100 row always green and confirm
    # test_degradations fails on it["pt100"].color.

    def test_last_check_row(self) -> None:
        it = items(last_check={"overall": "FAIL", "motor0": "FAIL", "pwm": "OK"})
        self.assertEqual(it["check"].color, "red")
        self.assertIn("motor0", it["check"].note)
        self.assertNotIn("check", items())

    def test_no_packet(self) -> None:
        rows = checkout_items(OnboardState(), link_ok=True)
        self.assertEqual([r.key for r in rows], ["link"])
        self.assertEqual(rows[0].color, "amber")


if __name__ == "__main__":
    unittest.main()
