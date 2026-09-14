from __future__ import annotations

import contextlib
import importlib.util
import io
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "associate_heaters.py"
SPEC = importlib.util.spec_from_file_location("associate_heaters", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
associate_heaters = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = associate_heaters
SPEC.loader.exec_module(associate_heaters)
hardware_setup = associate_heaters.hardware_setup


def pt100_ohms(temp_c: float) -> float:
    return 100.0 * (1.0 + 3.9083e-3 * temp_c - 5.775e-7 * temp_c * temp_c)


class FakeClock:
    def __init__(self) -> None:
        self.now = 5000.0

    def time(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += max(seconds, 0.0)


class FakeOnboard:
    """coatheal-onboard as seen through port 5000: eight specimens in a row
    (lumped heat capacity, loss to the room, some conduction to each
    neighbour), each PT100 lagging its specimen, behind a shuffled harness."""

    CAPACITY_J_PER_K = 15.0
    LOSS_W_PER_K = 0.06
    NEIGHBOUR_W_PER_K = 0.02
    PROBE_LAG_S = 8.0
    HEATER_W = 5.0
    ROOM_DRIFT_C_PER_S = 0.1 / 60.0
    STEP_S = 0.25

    def __init__(self, clock: FakeClock, harness: list[int], *, bench_mode: bool = True) -> None:
        self.clock = clock
        self.harness = list(harness)          # specimen -> RTD card terminal
        self.running_map = list(range(1, 9))  # the service's sequent_rtd_channels
        self.bench_mode = bench_mode
        self.heats = {h: [(h, 1.0)] for h in range(6)}  # heater -> [(specimen, share)]
        self.links = {s: [n for n in (s - 1, s + 1) if 0 <= n < 8] for s in range(8)}
        self.detached: set[int] = set()  # specimens whose probe hangs off them, reading the room
        self.faults: dict[int, tuple[float, str, float]] = {}  # terminal -> (from, fault, ohms)
        self.room = 23.0
        self.specimen = [23.0] * 8
        self.probe = [23.0] * 8
        self.hottest_probe = 23.0
        self.mode = "STANDBY"
        self.debug_armed = False
        self.test: tuple[int, float, float] | None = None
        self.sent: list[str] = []
        self._last = clock.now
        self._rng = random.Random(1234)

    def active(self) -> tuple[int | None, float]:
        if (self.test is not None and self.mode == "RUN" and self.debug_armed
                and self._last < self.test[2]):
            return self.test[0], self.test[1]
        return None, 0.0

    def _step(self) -> None:
        heater, duty = self.active()
        power = [0.0] * 8
        if heater is not None:
            for specimen, share in self.heats[heater]:
                power[specimen] += self.HEATER_W * duty * share
        flows = []
        for s in range(8):
            flow = power[s] - self.LOSS_W_PER_K * (self.specimen[s] - self.room)
            for n in self.links[s]:
                flow += self.NEIGHBOUR_W_PER_K * (self.specimen[n] - self.specimen[s])
            flows.append(flow)
        for s in range(8):
            self.specimen[s] += flows[s] * self.STEP_S / self.CAPACITY_J_PER_K
            follows = self.room if s in self.detached else self.specimen[s]
            self.probe[s] += (follows - self.probe[s]) * self.STEP_S / self.PROBE_LAG_S
        self.hottest_probe = max(self.hottest_probe, *self.probe)
        self.room += self.ROOM_DRIFT_C_PER_S * self.STEP_S
        self._last += self.STEP_S

    def _terminal(self, terminal: int) -> tuple[str, float]:
        fault = self.faults.get(terminal)
        if fault is not None and self.clock.now >= fault[0]:
            return fault[1], fault[2]
        probe = self.probe[self.harness.index(terminal)]
        # +0.6 ohm: 2-wire lead resistance.
        return "OK", pt100_ohms(probe) + 0.6 + self._rng.gauss(0.0, 0.03)

    def send(self, command: str, host: str, port: int) -> str:
        while self.clock.now - self._last >= self.STEP_S:
            self._step()
        self.sent.append(command)
        verb, *args = command.split()
        if verb == "STATUS":
            return (f"ACK,STATUS,phase=BOOT;mode={self.mode};manual_first=1;link_seen=0;"
                    f"link_loss_s=0;fallback_active=0;plan=none;"
                    f"bench_mode={int(self.bench_mode)};debug_armed={int(self.debug_armed)};"
                    "telemetry_target=;queue_depth=0;tick_hz=1;silence=0;simulated=0;"
                    "i2c_ok=1;sample_temp_ok=1;pwm_ok=1;stepper_ok=1;energy_wh=0;"
                    "energy_budget_wh=130;budget_exhausted=0;"
                    "seq0={motor=0;zeroed=0;running=0;paused=0;name=;step=0};"
                    "seq1={motor=1;zeroed=0;running=0;paused=0;name=;step=0}")
        if verb == "COMPONENTS":
            channels = "|".join(
                "S{}:ch{}:{}:{:.1f}".format(sample, terminal, *self._terminal(terminal))
                for sample, terminal in enumerate(self.running_map))
            return ("ACK,COMPONENTS,dps310=OK;dps310_error=NONE;dps310_age_ms=0;"
                    "sequent_rtd=OK;sequent_rtd_error=NONE;sequent_rtd_age_ms=0;"
                    "sequent_rtd_addr=0x40;sequent_rtd_burst=1;sequent_rtd_fw=3.0;"
                    f"sequent_rtd_valid=8/8;sequent_rtd_ch={channels};"
                    "max31865_1=OK;max31865_1_error=NONE;sample_valid_channels=8;"
                    "heated_channels_ok=1;simulated=0;pwm=OK;"
                    + ";".join(f"heater{i}=OK" for i in range(6))
                    + ";motor0=OK;motor1=OK;comms=OK")
        if verb == "GET_THERMAL":
            heater, duty = self.active()
            return "ACK,GET_THERMAL,target_min_c=0;target_max_c=80" + "".join(
                f";h{i}_target=-;h{i}_temp=-;h{i}_duty={duty if i == heater else 0:g}"
                for i in range(6))
        if verb == "ARM_DEBUG":
            if not self.bench_mode:
                return "NACK,ARM_DEBUG,bench mode required"
            self.debug_armed = True
            return "ACK,ARM_DEBUG,debug armed"
        if verb == "DISARM_DEBUG":
            self.debug_armed, self.test = False, None
            return "ACK,DISARM_DEBUG,debug disarmed"
        if verb == "ARM":
            if self.mode != "STANDBY":
                return "NACK,ARM,ARM requires STANDBY mode"
            self.mode = "RUN"
            return "ACK,ARM,mode=RUN;manual_control=1"
        if verb == "DISARM":
            self.mode, self.test = "STANDBY", None
            return "ACK,DISARM,mode=STANDBY"
        if verb == "HEATERS_OFF":
            self.test = None
            return "ACK,HEATERS_OFF,all heaters disabled"
        if verb == "HEATER_TEST":
            if not (self.bench_mode and self.debug_armed):
                return "NACK,HEATER_TEST,bench debug arm required"
            if self.mode != "RUN":
                return "NACK,HEATER_TEST,RUN mode required"
            heater, duty, seconds = int(args[0]), float(args[1]), float(args[2])
            if duty > 0.25:
                return "NACK,HEATER_TEST,duty exceeds heater.debug_max_duty"
            if not 0.0 < seconds <= 10.0:
                return "NACK,HEATER_TEST,duration exceeds heater.debug_max_seconds"
            self.test = (heater, duty, self.clock.now + seconds)
            return f"ACK,HEATER_TEST,heater={heater};duty={duty};seconds={seconds}"
        return "NACK,UNKNOWN,unknown command"


# Heater h warms specimen h; specimens 6 and 7 are unheated.
HARNESS = [3, 1, 6, 2, 8, 5, 4, 7]


class AssociateHeatersEndToEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.rig = FakeOnboard(self.clock, HARNESS)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.config = self.dir / "onboard.local.ini"
        self.config.write_text(hardware_setup.replace_ini(
            hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8"),
            {"runtime.bench_mode": "true", "heater.max_duty": "0.25"}), encoding="utf-8")
        self.original = self.config.read_text(encoding="utf-8")

    def run_script(self, *extra: str) -> tuple[int, str]:
        argv = ["--config", str(self.config), "--yes", "--no-restart",
                "--log", str(self.dir / "readings.csv"), *extra]
        output = io.StringIO()
        with mock.patch.object(associate_heaters, "service_config", return_value=None), \
                mock.patch.object(hardware_setup, "_check_with_binary", return_value=0), \
                contextlib.redirect_stdout(output):
            rc = associate_heaters.main(argv, send=self.rig.send,
                                        clock=self.clock.time, sleep=self.clock.sleep)
        return rc, output.getvalue()

    def assert_left_safe(self) -> None:
        self.assertIsNone(self.rig.active()[0])
        self.assertEqual(self.rig.mode, "STANDBY")
        self.assertFalse(self.rig.debug_armed)

    def test_recovers_a_shuffled_harness_and_writes_the_terminal_map(self) -> None:
        rc, output = self.run_script()
        self.assertEqual(rc, 0, output)
        values = hardware_setup._ini_values(self.config.read_text(encoding="utf-8"))
        self.assertEqual(values["sensor.sequent_rtd_channels"], "3,1,6,2,8,5,4,7")
        self.assertEqual(values["heater.temperature_channels"], "0,1,2,3,4,5")
        self.assertTrue(list(self.dir.glob("onboard.local.ini.bak.*")))
        self.assert_left_safe()
        tested = [int(c.split()[1]) for c in self.rig.sent if c.startswith("HEATER_TEST")]
        self.assertEqual(sorted(set(tested)), list(range(6)))
        self.assertEqual(tested, sorted(tested))  # one heater at a time, in order
        self.assertLess(self.rig.hottest_probe, 30.0)
        self.assertTrue((self.dir / "readings.csv").read_text(encoding="utf-8").startswith("t_s,"))

    def test_dry_run_reports_the_map_without_writing(self) -> None:
        rc, output = self.run_script("--dry-run")
        self.assertEqual(rc, 0, output)
        self.assertIn("S0 <- ch3", output)
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assert_left_safe()

    def test_a_heater_that_warms_nothing_blocks_the_write(self) -> None:
        self.rig.heats[2] = []
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("H2: no terminal warmed", output)
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assert_left_safe()

    def test_a_heater_warming_two_specimens_is_ambiguous(self) -> None:
        self.rig.heats[3] = [(3, 0.5), (4, 0.5)]
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertRegex(output, r"H3: ch(2|8) warmed to \d+% of ch(2|8)'s rise")
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assertLess(self.rig.hottest_probe, 30.0)  # stopped at 2x rise_c, not the time limit

    def test_a_probe_off_its_specimen_cannot_hand_the_heater_to_an_unheated_neighbour(self) -> None:
        # Worst case: H5's probe hangs in the air and H5's specimen conducts
        # only into unheated specimen 6, so ch4 (specimen 6's probe) is the
        # one clear, dominant responder. Only its slowness gives it away.
        self.rig.detached.add(5)
        self.rig.links = {0: [1], 1: [0, 2], 2: [1, 3], 3: [2, 4], 4: [3],
                          5: [6], 6: [5], 7: []}
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertRegex(output, r"H5: needed \d+ s to warm ch4, the others a median \d+ s")
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assert_left_safe()

    def test_a_terminal_jumping_past_the_abort_limit_stops_the_run(self) -> None:
        self.rig.faults[5] = (self.clock.now + 200.0, "OK", 223.0)
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("abort limit", output)
        self.assertIn("loose PT100 terminal", output)
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assertEqual(self.rig.sent[-3:], ["HEATERS_OFF", "DISARM_DEBUG", "DISARM"])
        self.assert_left_safe()

    def test_a_probe_dropping_out_stops_the_run(self) -> None:
        self.rig.faults[6] = (self.clock.now + 150.0, "OPEN", -366.0)
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("the PT100 on ch6 dropped out", output)
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assert_left_safe()

    def test_ctrl_c_mid_heat_leaves_heaters_off_and_disarmed(self) -> None:
        send = self.rig.send
        pulses = []

        def interrupt_on_third_pulse(command: str, host: str, port: int) -> str:
            if command.startswith("HEATER_TEST"):
                pulses.append(command)
                if len(pulses) == 3:
                    raise KeyboardInterrupt
            return send(command, host, port)

        self.rig.send = interrupt_on_third_pulse
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("STOPPED: interrupted", output)
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)
        self.assert_left_safe()

    def test_refuses_without_bench_mode_and_heats_nothing(self) -> None:
        self.rig.bench_mode = False
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("runtime.bench_mode is off", output)
        self.assertFalse([c for c in self.rig.sent if c.split()[0] in {"ARM", "ARM_DEBUG", "HEATER_TEST"}])

    def test_refuses_while_any_terminal_reads_no_probe(self) -> None:
        # Even an unheated specimen's probe: with a heater's own probe
        # missing, its neighbour's would be paired instead.
        self.rig.faults[7] = (0.0, "OPEN", 366.0)
        self.rig.faults[4] = (0.0, "SHORT", 0.6)
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("ch4, ch7 read no PT100", output)
        self.assertNotIn("HEATER_TEST", " ".join(self.rig.sent))

    def test_check_only_reports(self) -> None:
        rc, output = self.run_script("--check")
        self.assertEqual(rc, 0, output)
        self.assertIn("Ready: 6 heaters", output)
        self.assertEqual(self.rig.sent, ["STATUS", "COMPONENTS", "GET_THERMAL"])


class AssociateHeatersUnitTests(unittest.TestCase):
    def test_parse_terminals_reads_the_components_channel_list(self) -> None:
        terminals = associate_heaters.parse_terminals(
            "S0:ch3:OK:109.8|S1:ch2:OPEN:-366.0|S2:ch1:MISMATCH:nan|junk")
        self.assertEqual([(t.sample, t.channel, t.fault) for t in terminals],
                         [(0, 3, "OK"), (1, 2, "OPEN"), (2, 1, "MISMATCH")])
        self.assertEqual([t.conducting for t in terminals], [True, False, False])

    def test_pt100_c_inverts_the_pt100_curve(self) -> None:
        self.assertAlmostEqual(associate_heaters.pt100_c(100.0), 0.0, places=9)
        for temp in (-20.0, 25.0, 60.0):
            self.assertAlmostEqual(associate_heaters.pt100_c(pt100_ohms(temp)), temp, places=6)

    def test_compose_channel_map_keeps_unheated_terminals_in_current_order(self) -> None:
        association = {0: 1, 1: 2, 2: 3, 3: 5, 4: 6, 5: 8}
        self.assertEqual(
            associate_heaters.compose_channel_map(association, [1, 2, 3, 4, 5, 6, 7, 8], 8),
            [1, 2, 3, 5, 6, 8, 4, 7])
        self.assertEqual(
            associate_heaters.compose_channel_map(association, [8, 7, 6, 5, 4, 3, 2, 1], 8),
            [1, 2, 3, 5, 6, 8, 7, 4])

    def test_reply_fields_raises_on_a_nack_and_keeps_the_first_spelling(self) -> None:
        with self.assertRaises(associate_heaters.AssociationError):
            associate_heaters.reply_fields("ARM", "NACK,ARM,ARM requires STANDBY mode")
        fields = associate_heaters.reply_fields(
            "STATUS", "ACK,STATUS,mode=RUN;seq0={motor=0;mode=x};bench_mode=1")
        self.assertEqual(fields["mode"], "RUN")
        self.assertEqual(fields["bench_mode"], "1")


if __name__ == "__main__":
    unittest.main()
