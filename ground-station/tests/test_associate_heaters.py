from __future__ import annotations

import contextlib
import importlib.util
import io
import random
import re
import shlex
import subprocess
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


# The bench as built: specimen s (0..5) is heated by the heater on LINES[s]
# and read by the PT100 on card terminal HARNESS[s]; specimens 6 and 7 are
# unheated. The example config lists the lines in this order, so heater
# index h warms specimen h until an assignment reorders them.
LINES = [19, 13, 6, 5, 24, 23]
HARNESS = [3, 1, 6, 2, 8, 5, 4, 7]


class FakeOnboard:
    """coatheal-onboard as seen through port 5000: eight specimens in a row
    (lumped heat capacity, loss to the room, some conduction to each
    neighbour), each PT100 lagging its specimen, behind a shuffled harness.
    Heat follows the BCM line the service drives, so a reordered specimen
    list moves which specimen a heater index warms. Sized
    like the bench of 2026-09-14 at duty 0.25: a PT100 +1 C about 3 s after
    its heater comes on, still rising ~2 C after it goes off, a neighbour
    picking up a few tenths."""

    CAPACITY_J_PER_K = 1.4
    LOSS_W_PER_K = 0.05
    NEIGHBOUR_W_PER_K = 0.005
    PROBE_LAG_S = 4.0
    HEATER_W = 5.0
    ROOM_DRIFT_C_PER_S = 0.1 / 60.0
    STEP_S = 0.25

    def __init__(self, clock: FakeClock, harness: list[int], *, bench_mode: bool = True) -> None:
        self.clock = clock
        self.harness = list(harness)          # specimen -> RTD card terminal
        self.line_heats = {line: [(s, 1.0)] for s, line in enumerate(LINES)}  # BCM -> [(specimen, share)]
        # The heater lines and terminal map the service derived from its
        # config, re-read from config_path on every (fake) restart.
        self.output_lines = list(LINES)
        self.running_map = list(range(1, 9))
        self.config_path: Path | None = None
        self.restarts = 0
        self.bench_mode = bench_mode
        self.undriven: set[int] = set()  # heater indexes accepted but scheduled at duty 0
        self.busy_lines: set[int] = set()  # BCM lines the service cannot claim
        self.down = False                  # the service is not running
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
        self.timeline: list[tuple[float, str]] = []
        self._last = clock.now
        self._rng = random.Random(1234)

    def active(self) -> tuple[int | None, float]:
        if (self.test is not None and self.mode == "RUN" and self.debug_armed
                and self._last < self.test[2] and self.test[0] not in self.undriven):
            return self.test[0], self.test[1]
        return None, 0.0

    def _step(self) -> None:
        heater, duty = self.active()
        power = [0.0] * 8
        if heater is not None:
            for specimen, share in self.line_heats.get(self.output_lines[heater], []):
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

    def restart(self) -> None:
        """systemctl restart: a fresh process, disarmed, on the config as written."""
        self.restarts += 1
        self.mode, self.debug_armed, self.test = "STANDBY", False, None
        if self.config_path is not None:
            values = hardware_setup._ini_values(self.config_path.read_text(encoding="utf-8"))
            layout = hardware_setup.layout_from_values(values)
            self.output_lines = layout.heater_lines
            self.running_map = layout.channels

    def subprocess_run(self, args, **kwargs):
        if "systemctl" in args:
            self.restart()
        return subprocess.CompletedProcess(args, 0)

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
        if self.down:
            raise ConnectionRefusedError(111, "Connection refused")
        self.sent.append(command)
        self.timeline.append((self.clock.now, command))
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
                    + ";".join(f"heater{i}={'FAILED' if line in self.busy_lines else 'OK'}"
                               for i, line in enumerate(self.output_lines))
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


# assign's view of the same bench: each motor's specimens, heated first.
# Motor 0 pulls specimens 4, 1, 2, 3 and motor 1 pulls 0, 5, 6, 7.
ASSIGNMENT = ["--motor0", "ch8:24,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"]


class EndToEnd(unittest.TestCase):
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
        self.rig.config_path = self.config

    def run_script(self, *argv: str, restart: bool = False) -> tuple[int, str]:
        """Without a command word this is `auto`, as on the bench."""
        command = argv[0] if argv and argv[0] in associate_heaters.COMMANDS else ""
        options = ["--config", str(self.config)]
        if command in ("", "auto", "heat"):
            options += ["--log", str(self.dir / "readings.csv")]
        if command in ("", "auto", "assign"):
            options += ["--yes"] + ([] if restart else ["--no-restart"])
        full = ([command, *options, *argv[1:]] if command else [*options, *argv])
        output = io.StringIO()
        with mock.patch.object(hardware_setup, "BOOT_CONFIG_PATHS", (self.dir / "config.txt",)), \
                mock.patch.object(associate_heaters, "service_config", return_value=None), \
                mock.patch.object(hardware_setup, "_check_with_binary", return_value=0), \
                mock.patch.object(associate_heaters.subprocess, "run", self.rig.subprocess_run), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            rc = associate_heaters.main(full, send=self.rig.send,
                                        clock=self.clock.time, sleep=self.clock.sleep)
        return rc, output.getvalue()

    def values(self) -> dict[str, str]:
        return hardware_setup._ini_values(self.config.read_text(encoding="utf-8"))

    def layout(self):
        return hardware_setup.layout_from_values(self.values())

    def written_map(self) -> list[int]:
        return self.layout().channels

    def heater_windows(self) -> dict[int, tuple[float, float]]:
        """Heater -> (first pulse, the HEATERS_OFF that ended it)."""
        windows: dict[int, tuple[float, float]] = {}
        current = None
        for t, command in self.rig.timeline:
            if command.startswith("HEATER_TEST"):
                heater = int(command.split()[1])
                if heater != current:
                    self.assertIsNone(current, "a heater was started before the last one went off")
                    current = heater
                    windows[heater] = (t, t)
            elif command == "HEATERS_OFF" and current is not None:
                windows[current] = (windows[current][0], t)
                current = None
        return windows

    def assert_left_safe(self) -> None:
        self.assertIsNone(self.rig.active()[0])
        self.assertEqual(self.rig.mode, "STANDBY")
        self.assertFalse(self.rig.debug_armed)

    def assert_untouched(self) -> None:
        self.assertEqual(self.config.read_text(encoding="utf-8"), self.original)


class AutoTests(EndToEnd):
    def test_pairs_a_shuffled_harness_and_writes_the_map(self) -> None:
        rc, output = self.run_script()
        self.assertEqual(rc, 0, output)
        written = self.written_map()
        self.assertEqual(written[:6], [3, 1, 6, 2, 8, 5])  # S<h> <- the terminal H<h> warms
        self.assertEqual(sorted(written[6:]), [4, 7])       # the two unheated, in some order
        self.assertEqual(self.layout().heater_lines, [19, 13, 6, 5, 24, 23])
        self.assertEqual(self.values()["motor0.specimens"], "ch3:19,ch1:13,ch6:6,ch2:5")
        self.assertTrue(list(self.dir.glob("onboard.local.ini.bak.*")))
        self.assert_left_safe()
        windows = self.heater_windows()  # also: one heater at a time, each switched off
        self.assertEqual(list(windows), list(range(6)))
        self.assertLess(self.rig.hottest_probe, 30.0)
        self.assertIn("H0 (BCM 19) -> ch3", output)
        self.assertNotIn("NOT", output)
        self.assertIn(f"assign --motor0 ch3:19,ch1:13,ch6:6,ch2:5 --motor1 ch8:24,ch5:23,"
                      f"ch{written[6]},ch{written[7]}", output)
        self.assertTrue((self.dir / "readings.csv").read_text(encoding="utf-8").startswith("t_s,"))

    def test_restarts_the_service_and_checks_it_runs_the_new_map(self) -> None:
        rc, output = self.run_script(restart=True)
        self.assertEqual(rc, 0, output)
        self.assertEqual(self.rig.restarts, 1)
        self.assertEqual(self.rig.running_map, self.written_map())
        self.assertIn("Service restarted and runs the new assignment", output)
        # A rerun finds the same pairs in the running map and changes nothing.
        before = self.config.read_text(encoding="utf-8")
        rc, output = self.run_script(restart=True)
        self.assertEqual(rc, 0, output)
        self.assertIn("already carries this assignment", output)
        self.assertIn("The running service already uses it", output)
        self.assertEqual(self.rig.restarts, 1)
        self.assertEqual(self.config.read_text(encoding="utf-8"), before)

    def test_dry_run_reports_the_map_without_writing(self) -> None:
        rc, output = self.run_script("auto", "--dry-run", "--verbose")
        self.assertEqual(rc, 0, output)
        self.assertRegex(output, r"Motor 0: 4 specimens, 4 heated\n\s+Sample\s+Heater")
        self.assertRegex(output, r"S0\s+H0 BCM 19\s+ch3\s+1\s+measured")
        self.assertIn("(C above the reading at switch-on)", output)  # --verbose table
        self.assert_untouched()
        self.assert_left_safe()

    def test_a_heater_that_warms_nothing_is_left_out_and_the_rest_are_written(self) -> None:
        # Bench 2026-09-14: H2 warmed nothing on BCM 6, the other five paired.
        self.rig.line_heats.pop(6)
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("H2 (BCM 6) -> NOT PAIRED: no terminal warmed in 60 s", output)
        written = self.written_map()
        self.assertEqual([written[h] for h in (0, 1, 3, 4, 5)], [3, 1, 2, 8, 5])
        self.assertEqual(sorted(written), list(range(1, 9)))
        self.assertRegex(output, rf"S2\s+H2 BCM 6\s+ch{written[2]}\s+NOT measured")
        self.assertIn("H2 left out", output)
        start, end = self.heater_windows()[2]
        self.assertLessEqual(end - start, 63.0)  # the silent heater did not stay on
        self.assert_left_safe()

    def test_no_heater_warming_anything_writes_nothing(self) -> None:
        self.rig.line_heats = {}
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("nothing was written", output)
        self.assert_untouched()
        self.assert_left_safe()

    def test_a_heater_warming_two_specimens_is_left_out(self) -> None:
        self.rig.line_heats[5] = [(3, 0.5), (4, 0.5)]
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertRegex(output, r"H3 \(BCM 5\) -> NOT PAIRED: ch(2|8) \+\d\.\d C and "
                                 r"ch(2|8) \+\d\.\d C warmed together")
        written = self.written_map()
        self.assertEqual([written[h] for h in (0, 1, 2, 4, 5)], [3, 1, 6, 8, 5])
        start, end = self.heater_windows()[3]
        self.assertLess(end - start, 20.0)  # stopped at 2x rise_c, not the time limit
        self.assert_left_safe()

    def test_two_heaters_on_one_specimen_are_both_left_out(self) -> None:
        self.rig.line_heats[13] = [(0, 1.0)]
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("H0 and H1 both warmed ch3", output)
        self.assertIn("H0 (BCM 19) -> NOT PAIRED", output)
        self.assertIn("H1 (BCM 13) -> NOT PAIRED", output)
        written = self.written_map()
        self.assertEqual([written[h] for h in (2, 3, 4, 5)], [6, 2, 8, 5])
        self.assert_left_safe()

    def detach_probe_5_next_to_unheated_specimen_6(self) -> None:
        # Worst case: H5's probe hangs in the air and H5's specimen conducts
        # only into unheated specimen 6, whose probe (ch4) is then the one
        # terminal that warms.
        self.rig.detached.add(5)
        self.rig.links = {0: [1], 1: [0, 2], 2: [1, 3], 3: [2, 4], 4: [3],
                          5: [6], 6: [5], 7: []}

    def test_a_probe_off_its_specimen_is_not_paired_with_the_neighbour(self) -> None:
        self.detach_probe_5_next_to_unheated_specimen_6()
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("H5 (BCM 23) -> NOT PAIRED", output)
        self.assertNotIn("H5 (BCM 23) -> ch4", output)
        self.assertEqual(self.written_map()[:5], [3, 1, 6, 2, 8])
        self.assert_left_safe()

    def test_a_heater_far_slower_than_the_rest_is_not_trusted(self) -> None:
        # The same trap with specimens that conduct well: the neighbour does
        # reach the rise before the time limit, and only its slowness gives
        # it away.
        self.detach_probe_5_next_to_unheated_specimen_6()
        self.rig.NEIGHBOUR_W_PER_K = 0.03
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertRegex(output, r"H5 \(BCM 23\) -> NOT PAIRED: took \d+ s to warm ch4, "
                                 r"the others \d+ s")
        self.assert_left_safe()

    def test_each_heater_waits_until_the_last_pt100_stops_rising(self) -> None:
        # A PT100 keeps rising after its heater is off (bench 2026-09-14: up
        # to +2.3 C). Started too early, the next heater sees two terminals
        # warm together and pairs nothing. A slow probe on a specimen that
        # holds its heat (thin air at float) rises for longer than any fixed
        # pause: 15 s left five of these six heaters unpaired.
        for loss_w_per_k, lag_s in ((0.05, 4.0), (0.01, 25.0)):
            with self.subTest(loss_w_per_k=loss_w_per_k, probe_lag_s=lag_s):
                self.setUp()
                self.rig.LOSS_W_PER_K = loss_w_per_k
                self.rig.PROBE_LAG_S = lag_s
                rc, output = self.run_script()
                self.assertEqual(rc, 0, output)
                self.assertEqual(self.written_map()[:6], [3, 1, 6, 2, 8, 5])
                self.assertLess(self.rig.hottest_probe, 35.0)

    def test_a_heater_the_onboard_does_not_drive_is_reported_as_such(self) -> None:
        self.rig.undriven.add(4)
        rc, output = self.run_script()
        self.assertEqual(rc, 1, output)
        self.assertIn("H4 (BCM 24) -> NOT PAIRED: the onboard accepted HEATER_TEST but "
                      "applied duty 0", output)
        start, end = self.heater_windows()[4]
        self.assertLess(end - start, 10.0)
        self.assert_left_safe()

    def test_a_terminal_jumping_past_the_abort_limit_stops_the_run(self) -> None:
        self.rig.faults[5] = (self.clock.now + 100.0, "OK", 223.0)
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("abort limit", output)
        self.assertIn("loose PT100 terminal", output)
        self.assert_untouched()
        self.assertEqual(self.rig.sent[-3:], ["HEATERS_OFF", "DISARM_DEBUG", "DISARM"])
        self.assert_left_safe()

    def test_a_probe_dropping_out_stops_the_run(self) -> None:
        self.rig.faults[6] = (self.clock.now + 80.0, "OPEN", -366.0)
        rc, output = self.run_script()
        self.assertEqual(rc, 2, output)
        self.assertIn("the PT100 on ch6 stopped reading", output)
        self.assert_untouched()
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
        self.assert_untouched()
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


class AssignTests(EndToEnd):
    def test_show_prints_the_table_and_the_assign_command_that_reproduces_it(self) -> None:
        rc, output = self.run_script("show")
        self.assertEqual(rc, 0, output)
        self.assertRegex(output, r"S0\s+H0 BCM 19\s+ch1\s+1\s+OK 10\d\.\d ohm 2\d\.\d C")
        self.assertRegex(output, r"Motor 1: 4 specimens, 2 heated\n\s+Sample.*\n\s+S4\s+H4 BCM 24\s+ch5\s+2")
        self.assertRegex(output, r"S7\s+unheated\s+ch8")
        self.assertNotIn("!", output)
        command = next(line.strip() for line in output.splitlines() if " assign " in line)
        self.assertEqual(command, "python3 scripts/associate_heaters.py assign "
                                  "--motor0 ch1:19,ch2:13,ch3:6,ch4:5 --motor1 ch5:24,ch6:23,ch7,ch8")
        self.assertFalse([c for c in self.rig.sent if c not in ("STATUS", "COMPONENTS")])
        rc, output = self.run_script(*shlex.split(command)[2:])
        self.assertEqual(rc, 0, output)
        self.assertIn("already carries this assignment", output)
        self.assert_untouched()

    def test_moves_specimens_between_motor_groups_and_auto_agrees(self) -> None:
        rc, output = self.run_script("assign", *ASSIGNMENT, restart=True)
        self.assertEqual(rc, 0, output)
        values = self.values()
        self.assertEqual(values["motor0.specimens"], "ch8:24,ch1:13,ch6:6,ch2:5")
        self.assertEqual(values["motor1.specimens"], "ch3:19,ch5:23,ch4,ch7")
        self.assertEqual(self.layout().heater_lines, [24, 13, 6, 5, 19, 23])
        self.assertEqual(self.written_map(), [8, 1, 6, 2, 3, 5, 4, 7])
        self.assertRegex(output, r"S4\s+H4 BCM 19\s+ch3\s+2\s+.*\(heater was BCM 24, "
                                 r"PT100 was ch5\)")
        self.assertNotIn("Heater lines changed", output)  # the same six lines, reordered
        self.assertEqual(self.rig.restarts, 1)
        self.assertEqual(self.rig.output_lines, [24, 13, 6, 5, 19, 23])
        self.assertIn("Service restarted and runs the new assignment", output)
        # Heat through the new numbering finds the same pairs: H0 is now
        # BCM 24, which warms ch8.
        rc, output = self.run_script(restart=True)
        self.assertEqual(rc, 0, output)
        self.assertIn("H0 (BCM 24) -> ch8", output)
        self.assertIn("already carries this assignment", output)
        self.assertEqual(self.rig.restarts, 1)
        self.assert_left_safe()

    def test_show_without_the_service_prints_the_config(self) -> None:
        self.rig.down = True
        rc, output = self.run_script("show")
        self.assertEqual(rc, 0, output)
        self.assertIn("the onboard does not answer", output)
        self.assertRegex(output, r"S5\s+H5 BCM 23\s+ch6\n")
        self.assertIn("assign --motor0 ch1:19,", output)

    def test_show_names_a_config_the_service_has_not_loaded(self) -> None:
        self.config.write_text(hardware_setup.replace_ini(
            self.original, {"motor0.specimens": "ch3:19,ch1:13,ch6:6,ch2:5",
                            "motor1.specimens": "ch8:24,ch5:23,ch4,ch7"}), encoding="utf-8")
        rc, output = self.run_script("show")
        self.assertEqual(rc, 0, output)
        self.assertIn("! the service reads terminals 1,2,3,4,5,6,7,8, not the config's", output)

    def test_dry_run_writes_nothing(self) -> None:
        rc, output = self.run_script("assign", *ASSIGNMENT, "--dry-run")
        self.assertEqual(rc, 0, output)
        self.assertIn("--dry-run: nothing written", output)
        self.assert_untouched()

    def test_a_line_the_service_cannot_claim_fails_the_restart_check(self) -> None:
        self.rig.busy_lines.add(16)
        rc, output = self.run_script("assign", "--motor0", "ch8:24,ch1:13,ch6:16,ch2:5",
                                     *ASSIGNMENT[2:], restart=True)
        self.assertEqual(rc, 2, output)
        self.assertIn("did not claim H2 (BCM 16)", output)

    def test_a_heater_moved_to_another_line(self) -> None:
        # H2's heater is really on BCM 16: assign it there, restart, and
        # heat that heater to check.
        self.rig.line_heats[16] = self.rig.line_heats.pop(6)
        motor0 = "ch8:24,ch1:13,ch6:16,ch2:5"
        rc, output = self.run_script("assign", "--motor0", motor0, *ASSIGNMENT[2:], restart=True)
        self.assertEqual(rc, 0, output)
        self.assertEqual(self.layout().heater_lines, [24, 13, 16, 5, 19, 23])
        self.assertIn("Heater lines changed (BCM 6 out, BCM 16 in): run coatheal-deploy", output)
        rc, output = self.run_script("heat", "H2")
        self.assertEqual(rc, 0, output)
        self.assertIn("H2 (BCM 16) warms ch6, which the config reads as S2 on motor 0: "
                      "they agree", output)
        self.assert_left_safe()

    def test_refuses_what_the_onboard_cannot_run(self) -> None:
        cases = {
            "list 7 heated specimens; hardware.heater_count is 6":
                ["--motor0", "ch8:24,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4:12,ch7"],
            "ch8 is listed twice in motor0.specimens / motor1.specimens":
                ["--motor0", "ch8:24,ch8:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"],
            "BCM 24 heats two specimens in motor0.specimens / motor1.specimens":
                ["--motor0", "ch8:24,ch1:24,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"],
            "motor0.specimens: ch9 is not a card terminal (ch1..ch8)":
                ["--motor0", "ch9:24,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"],
            "list 7 specimens; hardware.sample_count is 8":
                ["--motor0", "ch8:24,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4"],
            "'8:24': give a PT100 terminal":
                ["--motor0", "8:24,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"],
            "'ch8:H0': give a PT100 terminal":
                ["--motor0", "ch8:H0,ch1:13,ch6:6,ch2:5", "--motor1", "ch3:19,ch5:23,ch4,ch7"],
        }
        for message, argv in cases.items():
            with self.subTest(message):
                rc, output = self.run_script("assign", *argv)
                self.assertEqual(rc, 2, output)
                self.assertIn(message, output)
                self.assert_untouched()
        self.assertEqual(self.rig.sent, [])

    def test_uneven_groups_are_written_and_auto_agrees(self) -> None:
        # Three heated and one unheated specimen per motor: H3 is now motor
        # 1's first specimen, S4, and reads ch3.
        rc, output = self.run_script("assign", "--motor0", "ch8:24,ch1:13,ch6:6,ch4",
                                     "--motor1", "ch3:19,ch5:23,ch2:5,ch7", restart=True)
        self.assertEqual(rc, 0, output)
        layout = self.layout()
        self.assertEqual(layout.heater_samples, [0, 1, 2, 4, 5, 6])
        self.assertEqual(layout.clicks, [0, 4])
        self.assertRegex(output, r"Motor 0: 4 specimens, 3 heated")
        self.assertRegex(output, r"S3\s+unheated\s+ch4\s+.*\(heater was BCM 5\)")
        self.assertRegex(output, r"S4\s+H3 BCM 19\s+ch3\s+2\s+")
        self.assertRegex(output, r"S6\s+H5 BCM 5\s+ch2\s+.*\(was unheated, PT100 was ch7\)")
        rc, output = self.run_script("auto", "--dry-run")
        self.assertEqual(rc, 0, output)
        self.assertIn("H3 (BCM 19) -> ch3", output)
        self.assertNotIn("NOT", output)

    def test_a_config_from_before_the_specimen_keys_is_converted_on_write(self) -> None:
        template = self.original
        legacy = "\n".join(line for line in template.splitlines()
                           if not line.startswith(hardware_setup.SPECIMEN_KEYS)) + "\n"
        legacy += ("heater.output_lines=19,13,6,5,24,23\n"
                   "sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8\n"
                   "motor0.samples=0,1,2,3\nmotor1.samples=4,5,6,7\n")
        self.config.write_text(legacy, encoding="utf-8")
        rc, output = self.run_script("show")
        self.assertEqual(rc, 0, output)
        self.assertIn("still uses the index-based layout keys", output)
        rc, output = self.run_script("assign", *ASSIGNMENT)
        self.assertEqual(rc, 0, output)
        self.assertIn("sensor.sequent_rtd_channels: 1,2,3,4,5,6,7,8 -> (removed", output)
        values = self.values()
        for key in hardware_setup.LAYOUT_KEYS:
            self.assertNotIn(key, values)
        self.assertEqual(values["motor1.specimens"], "ch3:19,ch5:23,ch4,ch7")
        self.assertEqual(hardware_setup.validate_candidate(self.config.read_text(encoding="utf-8")), [])

    def test_warns_when_config_txt_no_longer_holds_the_heater_lines_low(self) -> None:
        # On the Pi: the boot block written by the last deploy, then a heater
        # moved to BCM 16 without a deploy.
        boot = self.dir / "config.txt"
        values = self.values()
        boot.write_text(hardware_setup.upsert_boot_gpio_block(
            "dtparam=spi=on\n",
            hardware_setup.render_boot_gpio_block(hardware_setup.boot_gpio_lines(values))),
            encoding="utf-8")
        rc, output = self.run_script("show")
        self.assertNotIn("!", output)
        self.rig.line_heats[16] = self.rig.line_heats.pop(6)
        rc, output = self.run_script("assign", "--motor0", "ch1:19,ch2:13,ch3:16,ch4:5",
                                     "--motor1", "ch5:24,ch6:23,ch7,ch8")
        self.assertEqual(rc, 0, output)
        self.assertIn(f"! {boot} holds gpio=5,6,13,19,23,24=op,dl,pd from boot, but this "
                      "config needs gpio=5,13,16,19,23,24=op,dl,pd: run coatheal-deploy, "
                      "then reboot", output)
        rc, output = self.run_script("show")
        self.assertIn("run coatheal-deploy, then reboot", output)

    def test_a_line_the_hat_owns_is_refused_by_the_validator(self) -> None:
        rc, output = self.run_script("assign", "--motor0", "ch8:24,ch1:13,ch6:17,ch2:5",
                                     *ASSIGNMENT[2:])
        self.assertEqual(rc, 1, output)
        self.assertIn("line 17 used by reserved: sequent_hat rs485_dir", output)
        self.assertIn("nothing written", output)
        self.assert_untouched()


class DebugTests(EndToEnd):
    def test_heat_prints_every_terminal_and_names_the_pt100(self) -> None:
        rc, output = self.run_script("heat", "H0")
        self.assertEqual(rc, 0, output)
        self.assertIn("H0 (BCM 19) warms ch3, but the config reads S0 (motor 0) from ch1", output)
        rows = [line for line in output.splitlines() if re.match(r"\s+\d+s(\s+[-+]\d+\.\d){8}", line)]
        self.assertGreater(len(rows), 5)
        self.assertTrue(any(line.endswith("H0 0.25") for line in rows))
        self.assertTrue(any(line.endswith("(off)") for line in rows))  # after it went off
        self.assertEqual({c.split()[1] for c in self.rig.sent if c.startswith("HEATER_TEST")}, {"0"})
        self.assert_untouched()
        self.assert_left_safe()

    def test_heat_a_silent_heater_says_so(self) -> None:
        self.rig.line_heats.pop(6)
        rc, output = self.run_script("heat", "H2", "--seconds", "20", "--after-s", "0")
        self.assertEqual(rc, 1, output)
        self.assertIn("H2 (BCM 6) did not pair: no terminal warmed in 20 s", output)
        start, end = self.heater_windows()[2]
        self.assertLessEqual(end - start, 22.0)
        self.assert_left_safe()

    def test_heat_runs_with_a_probe_missing_elsewhere(self) -> None:
        self.rig.faults[7] = (0.0, "OPEN", 366.0)
        rc, output = self.run_script("heat", "0", "--after-s", "0")
        self.assertEqual(rc, 0, output)
        self.assertIn("ch7 read no PT100 and are not watched", output)
        self.assertIn("H0 (BCM 19) warms ch3", output)
        self.assert_left_safe()

    def test_heat_refuses_an_unknown_heater(self) -> None:
        rc, output = self.run_script("heat", "H6")
        self.assertEqual(rc, 2, output)
        self.assertIn("No heater 'H6': H0..H5", output)
        self.assertEqual(self.rig.sent, [])

    def test_watch_prints_every_terminal_and_commands_nothing(self) -> None:
        # Someone heats H0 from the ground station meanwhile.
        self.rig.mode, self.rig.debug_armed = "RUN", True
        self.rig.test = (0, 0.25, self.clock.now + 30.0)
        rc, output = self.run_script("watch", "--seconds", "8")
        self.assertEqual(rc, 0, output)
        self.assertRegex(output, r"sample(\s+M[01]S\d){8}")
        rows = [line for line in output.splitlines() if re.match(r"\s+\d+s(\s+[-+]\d+\.\d){8}", line)]
        self.assertEqual(len(rows), 8)
        self.assertTrue(rows[-1].endswith("H0 0.25"))
        ch3 = float(rows[-1].split()[3])
        self.assertGreater(ch3, 1.0)
        self.assertEqual(set(self.rig.sent), {"COMPONENTS", "GET_THERMAL"})


def result(heater: int, channel: int, seconds: float, verdict: str = "paired"):
    return associate_heaters.HeaterResult(heater, heater, verdict, channel, 2.5,
                                          (channel % 8 + 1, 0.1), seconds)


def example_layout():
    return hardware_setup.layout_from_values(
        hardware_setup._ini_values(hardware_setup.EXAMPLE_CONFIG.read_text(encoding="utf-8")))


class UnitTests(unittest.TestCase):
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

    def test_compose_layout_places_the_pairs_and_keeps_the_rest(self) -> None:
        compose = associate_heaters.compose_layout
        layout = example_layout()  # schematic order: S<i> on ch<i+1>
        # Bench 2026-09-14: H2 unpaired keeps ch3, which no paired heater took;
        # the unheated specimens lost ch7/ch8 and get the leftovers in order.
        new = compose({0: 8, 1: 2, 3: 4, 4: 5, 5: 7}, layout)
        self.assertEqual(new.channels, [8, 2, 3, 4, 5, 7, 1, 6])
        self.assertEqual(new.heater_lines, layout.heater_lines)
        self.assertEqual(compose({}, layout), layout)
        # Uneven groups: H3 is S4, so its terminal lands on S4.
        uneven = associate_heaters.build_assignment(
            ["ch1:19,ch2:13,ch3:6,ch4", "ch5:5,ch6:24,ch7:23,ch8"], 8, 6)
        self.assertEqual(compose({3: 7}, uneven).channels, [1, 2, 3, 4, 7, 6, 5, 8])

    def test_resolve_leaves_out_shared_terminals_and_a_slow_heater(self) -> None:
        results = [result(0, 8, 6.0), result(1, 2, 5.0), result(2, 3, 40.0),
                   result(3, 4, 6.0), result(4, 4, 7.0), result(5, 7, 5.0, "no response")]
        association, problems = associate_heaters.resolve(results)
        self.assertEqual(association, {0: 8, 1: 2})
        self.assertEqual(sorted(problems), [2, 3, 4, 5])
        self.assertIn("took 40 s to warm ch3", problems[2])
        self.assertIn("H3 and H4 both warmed ch4", problems[3])
        self.assertIn("no terminal warmed", problems[5])

    def test_parse_specimens_reads_terminals_and_heater_lines(self) -> None:
        Specimen = hardware_setup.Specimen
        self.assertEqual(hardware_setup.parse_specimens(" CH8:19, ch2:bcm13,ch1 ,"),
                         [Specimen(8, 19), Specimen(2, 13), Specimen(1, None)])
        with self.assertRaises(ValueError):
            hardware_setup.parse_specimens("ch8:19,,ch1")

    def test_build_assignment_keeps_the_order_given(self) -> None:
        layout = associate_heaters.build_assignment(
            ["ch4,ch8:24,ch1:13,ch6:6", "ch3:19,ch7,ch5:23,ch2:5"], 8, 6)
        self.assertEqual(layout.channels, [4, 8, 1, 6, 3, 7, 5, 2])
        self.assertEqual(layout.heater_lines, [24, 13, 6, 19, 23, 5])
        self.assertEqual(layout.heater_samples, [1, 2, 3, 4, 6, 7])
        self.assertEqual(layout.clicks, [0, 4])  # an unheated specimen can carry the click
        with self.assertRaises(ValueError):
            associate_heaters.build_assignment(["ch4,ch8:24,ch1:13", "ch3:19,ch7,ch5:23,ch2:5"], 8, 6)

    def test_assign_command_round_trips(self) -> None:
        layout = associate_heaters.build_assignment(
            ["ch8:24,ch1:13,ch6:16,ch2:5", "ch3:19,ch5:23,ch4,ch7"], 8, 6)
        argv = shlex.split(associate_heaters.assign_command(layout))
        motor0, motor1 = argv[argv.index("--motor0") + 1], argv[argv.index("--motor1") + 1]
        self.assertEqual(associate_heaters.build_assignment([motor0, motor1], 8, 6), layout)

    def test_reply_fields_raises_on_a_nack_and_keeps_the_first_spelling(self) -> None:
        with self.assertRaises(associate_heaters.AssociationError):
            associate_heaters.reply_fields("ARM", "NACK,ARM,ARM requires STANDBY mode")
        fields = associate_heaters.reply_fields(
            "STATUS", "ACK,STATUS,mode=RUN;seq0={motor=0;mode=x};bench_mode=1")
        self.assertEqual(fields["mode"], "RUN")
        self.assertEqual(fields["bench_mode"], "1")


if __name__ == "__main__":
    unittest.main()
