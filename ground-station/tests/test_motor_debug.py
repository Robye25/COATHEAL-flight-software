"""MOTOR_DEBUG parsing and the motion estimator."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.motor_debug import MotionEstimator, MotorDebugSample, mscnt_delta  # noqa: E402

BODY = ("motor=1;sw_pos=312;sw_tgt=800;sw_hz=100;us=4;enabled=1;moving=1;holding=0;pulses=312;missed=0;"
        "xactual=79872;xtarget=204800;vactual={vactual};mscnt={mscnt};tstep=120;drv_status=0x80094000;stst={stst};"
        "cs_actual=9;sg_result=18;stallguard=0;ot=0;otpw=0;s2ga=0;s2gb=0;ola=0;olb=0;s2vsa=0;s2vsb=0;stealth=1;"
        "fsactive=0;rampstat=0x0;vzero=0;pos_reached=0;vel_reached=1;status_sg=0;ioin=0x30000000;drv_enn={drv_enn};"
        "sd_mode={sd_mode};version=0x30;gstat=0x0;chopconf=0x06010043;toff={toff};mres=6;usteps=4")


def body(mscnt=0, stst=0, drv_enn=0, sd_mode=0, toff=3, vactual=35000):
    return BODY.format(mscnt=mscnt, stst=stst, drv_enn=drv_enn, sd_mode=sd_mode, toff=toff, vactual=vactual)


class ParseTests(unittest.TestCase):
    def test_parse(self) -> None:
        s = MotorDebugSample.parse(body(mscnt=544), t=1.0)
        self.assertEqual(s.motor, 1)
        self.assertEqual(s.mscnt, 544)
        self.assertEqual(s.xactual, 79872)
        self.assertEqual(s.usteps, 4)
        self.assertIs(s.enabled, True)
        self.assertIs(s.drv_enn, False)
        self.assertEqual(s.faults, ())
        self.assertEqual(s.sw_hz, 100.0)

    def test_delta_wraps(self) -> None:
        self.assertEqual(mscnt_delta(1000, 20), 44)
        self.assertEqual(mscnt_delta(20, 1000), -44)
        self.assertEqual(mscnt_delta(100, 164), 64)


class EstimatorTests(unittest.TestCase):
    def test_moving_at_100_full_steps_per_second(self) -> None:
        est = MotionEstimator(mm_per_rev=1.5)
        # 100 full-steps/s = 25600 MSCNT counts/s; sampled at 2 Hz -> 12800 per sample (mod 1024 = 512).
        mscnt = 0
        for i in range(8):
            e = est.add(MotorDebugSample.parse(body(mscnt=mscnt), t=i * 0.5))
            mscnt = (mscnt + 12800) % 1024
        # A 512-count step per half second is ambiguous mod 1024 -- it is what
        # 100 Hz looks like at 2 Hz sampling; the estimator must not be fooled
        # into "frozen".
        self.assertEqual(e.color, "green", e.verdict)
        self.assertGreater(e.sequencer_full_steps_s, 3.0)

    def test_slow_motion_is_visible(self) -> None:
        est = MotionEstimator(mm_per_rev=1.5)
        # 2 full-steps/s (very slow): 512 counts/s -> 256 per 0.5 s sample.
        for i in range(8):
            e = est.add(MotorDebugSample.parse(body(mscnt=(i * 256) % 1024), t=i * 0.5))
        self.assertEqual(e.color, "green")
        self.assertAlmostEqual(e.sequencer_full_steps_s, 2.0, places=3)
        self.assertAlmostEqual(e.rev_s, 0.01, places=4)
        self.assertAlmostEqual(e.mm_s, 0.015, places=4)
        self.assertAlmostEqual(e.travel_full_steps, 7.0, places=3)

    def test_commanded_but_frozen(self) -> None:
        est = MotionEstimator()
        for i in range(6):
            e = est.add(MotorDebugSample.parse(body(mscnt=32), t=i * 0.5))
        self.assertEqual(e.color, "red")
        self.assertIn("NOT STEPPING", e.verdict)

    # MUTATION: make _verdict return ("MOVING", "green") whenever last.moving
    # and confirm test_commanded_but_frozen fails on color.

    def test_power_stage_off_and_strap(self) -> None:
        est = MotionEstimator()
        e = est.add(MotorDebugSample.parse(body(mscnt=32, drv_enn=1, stst=1, vactual=0).replace("moving=1", "moving=0"), t=0.0))
        self.assertEqual(e.color, "gray")
        self.assertIn("power stage off", e.verdict)
        est.reset()
        e = est.add(MotorDebugSample.parse(body(sd_mode=1), t=0.0))
        self.assertEqual(e.color, "red")
        self.assertIn("SD_MODE", e.verdict)

    def test_chip_reset_is_named_first(self) -> None:
        est = MotionEstimator()
        e = est.add(MotorDebugSample.parse(body().replace("gstat=0x0", "gstat=0x1"), t=0.0))
        self.assertEqual(e.color, "red")
        self.assertIn("CHIP RESET", e.verdict)
        est.reset()
        e = est.add(MotorDebugSample.parse(body(mscnt=0) + ";resets=2", t=0.0))
        self.assertIn("reset ×2", e.verdict)

    def test_fault_flags(self) -> None:
        est = MotionEstimator()
        e = est.add(MotorDebugSample.parse(body().replace("ola=0", "ola=1"), t=0.0))
        self.assertEqual(e.color, "red")
        self.assertIn("ola", e.verdict)


if __name__ == "__main__":
    unittest.main()
