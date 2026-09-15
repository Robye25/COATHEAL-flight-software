#!/usr/bin/env python3
"""Put the bench wiring of heaters, PT100s and motor groups into the onboard
config: measure the heater/PT100 pairs by heat, debug one heater or the live
readings, or set the whole assignment by hand.

The software pairs heater H<i> with logical sample S<i>, pulls S0-S3 with
motor 0 and S4-S7 with motor 1 (S6 and S7 unheated), and reads the MAX31865
clicks on S0 and S4 -- and the ground station assumes the same. The harness
does not follow the schematic, so a specimen is placed in that frame through
two wiring maps: the BCM line of its heater goes into heater.output_lines[i]
and the card terminal of its PT100 into sensor.sequent_rtd_channels[i].
migrate-config (every coatheal-deploy) keeps both.

Run it on the Pi with coatheal-onboard running:

    python3 scripts/associate_heaters.py show      # the assignment, live readings
    python3 scripts/associate_heaters.py watch     # every PT100 once a second
    python3 scripts/associate_heaters.py heat H2   # one heater on, every PT100 printed
    python3 scripts/associate_heaters.py auto      # measure every pair, write (default)
    python3 scripts/associate_heaters.py assign \\
        --motor0 ch8:19,ch2:13,ch3:6,ch4:5 --motor1 ch5:24,ch7:23,ch1,ch6

assign takes each motor's specimens as a PT100 card terminal and, for a
heated specimen, the BCM line of its heater. Heated specimens take the
motor's heated samples in the order given, so list the specimen on the
MAX31865 click first; `show` prints the command for the current config.

Heating (heat, auto) needs runtime.bench_mode=true and the motors idle. Each
HEATER_TEST pulse lapses within seconds on its own, so a dead script cannot
leave a heater on. auto, per heater: waits until no PT100 is still warming,
heats until one terminal has warmed by 2 C while the others have not, and
switches the heater off. The pairs that resolve are written even when a
heater is left out (it warms nothing, warms two terminals alike, shares one
with another heater, or is far slower than the rest); its sample keeps its
terminal unless a paired heater took it. Heat cannot tell motor groups,
which unheated terminal is S6, or where the clicks are: fix those with
assign.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import shutil
import signal
import statistics
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hardware_setup  # noqa: E402  (shared INI and command helpers)

ENV_FILE = Path("/etc/coatheal/env")
SERVICE = "coatheal-onboard.service"
SCRIPT = "python3 scripts/associate_heaters.py"
COMMANDS = ("show", "watch", "heat", "auto", "assign")
CARD_TERMINALS = tuple(range(1, 9))
MOTOR_COUNT = 2
# What the ground station assumes (ground-station/app/gui/state.py:
# MOTOR_SAMPLES and RESISTANCE_SAMPLES; HEATER_SAMPLE is heater i -> sample i).
GS_MOTOR_SAMPLES = ((0, 1, 2, 3), (4, 5, 6, 7))
GS_CLICK_SAMPLES = (0, 4)
# Faults whose resistance still comes from a conducting probe. MISMATCH (the
# card's temperature disagrees with its own resistance) still tracks heat;
# OPEN and SHORT report a sentinel that cannot.
CONDUCTING = frozenset({"OK", "MISMATCH"})
# sequent_rtd_error after a successful card read. Anything else means the
# card stopped answering and sequent_rtd_ch is the last good, stale reading.
FRESH_RTD_ERRORS = frozenset({"NONE", "PARTIAL_CHANNELS", "NO_VALID_CHANNELS"})


class AssociationError(Exception):
    """A precondition failed, or the run was stopped for safety."""


@dataclass
class Settings:
    duty: float = 0.25
    # A terminal is the heater's once it reads rise_c above its reading at
    # switch-on while every other terminal stays within
    # max(noise_c, rise / dominance): a neighbour warming by conduction, or
    # the room drifting, must not pass for the heated specimen. COMPONENTS
    # reports 0.1 ohm, about 0.26 C per step.
    rise_c: float = 2.0
    dominance: float = 3.0
    noise_c: float = 0.5
    # Bench 2026-09-14: every connected heater moved its PT100 by +1 C within
    # 4 s at duty 0.25. A heater that has warmed nothing after this long is
    # not warming a PT100 on the card.
    max_heat_s: float = 60.0
    abort_c: float = 50.0         # any terminal this hot stops the run
    # HEATER_TEST length. Re-sent every pulse_s / 2.5 while heating, so the
    # heater drops within pulse_s of this script dying.
    pulse_s: float = 5.0
    poll_s: float = 1.0
    # Before each heater: until no terminal has warmed faster than
    # steady_c_per_s over the last steady_s, at most max_wait_s. Bench
    # 2026-09-14: a PT100 kept rising ~2 C after its heater went off. One
    # 0.1 ohm step inside the window stays under the limit.
    steady_s: float = 15.0
    steady_c_per_s: float = 0.03
    max_wait_s: float = 120.0
    duty_grace_s: float = 3.0     # a command lands on the next control tick


@dataclass(frozen=True)
class Terminal:
    sample: int      # logical sample the running map feeds from this terminal
    channel: int     # RTD card terminal 1..8, as labelled on the HAT
    fault: str       # OK / OPEN / SHORT / MISMATCH
    ohms: float

    @property
    def conducting(self) -> bool:
        return self.fault in CONDUCTING and math.isfinite(self.ohms)


@dataclass
class Reading:
    t: float
    raw: dict[int, float]     # C per terminal, as read
    temps: dict[int, float]   # median of the terminal's last three reads: no single spikes
    duties: list[float]       # duty the onboard applies per heater (GET_THERMAL)


@dataclass(frozen=True)
class HeaterResult:
    heater: int
    line: int
    verdict: str                          # paired / no response / ambiguous / not driven
    channel: Optional[int]                # warmest terminal when the heater went off
    rise_c: float                         # its rise over the reading at switch-on
    runner_up: Optional[tuple[int, float]]
    seconds: float                        # heating time


@dataclass
class Mapping:
    """The wiring the config states, per logical sample."""
    heater_lines: list[int]            # heater.output_lines: BCM line of H<i>
    channels: list[int]                # sensor.sequent_rtd_channels: card terminal of S<i>
    motors: list[list[int]]            # motor<n>.samples
    clicks: list[int]                  # sensor.max31865_sample_indices
    temperature_channels: list[int]    # heater.temperature_channels

    @property
    def heater_count(self) -> int:
        return len(self.heater_lines)

    def motor_of(self, sample: int) -> Optional[int]:
        return next((m for m, samples in enumerate(self.motors) if sample in samples), None)


@dataclass(frozen=True)
class Specimen:
    channel: int             # card terminal of its PT100
    line: Optional[int]      # BCM line of its heater; None when unheated


def reply_fields(command: str, reply: str) -> dict[str, str]:
    """`ACK,<CMD>,k=v;k=v` -> {k: v}. A NACK (or no reply) raises."""
    parts = reply.split(",", 2)
    if parts[0] != "ACK":
        raise AssociationError(f"{command} -> {reply or 'no reply'}")
    fields: dict[str, str] = {}
    for item in (parts[2] if len(parts) > 2 else "").split(";"):
        key, sep, value = item.partition("=")
        if sep:
            # STATUS nests `seq0={motor=0;running=0;...}`: first spelling wins.
            fields.setdefault(key.strip(), value.strip())
    return fields


def parse_terminals(value: str) -> list[Terminal]:
    """COMPONENTS' `sequent_rtd_ch=S0:ch1:OK:109.8|S1:ch2:OPEN:-366.0|...`."""
    terminals = []
    for entry in value.split("|"):
        pieces = entry.split(":")
        if (len(pieces) != 4 or not pieces[0].startswith("S")
                or not pieces[1].startswith("ch")):
            continue
        try:
            terminals.append(Terminal(int(pieces[0][1:]), int(pieces[1][2:]),
                                      pieces[2], float(pieces[3])))
        except ValueError:
            continue
    return terminals


def pt100_c(ohms: float) -> float:
    """Callendar-Van Dusen inverse without the sub-zero C term (negligible at
    bench temperature). A 2-wire probe reads high by its lead resistance,
    which cancels out of every rise this script measures."""
    a, b = 3.9083e-3, -5.775e-7
    return (-a + math.sqrt(a * a - 4.0 * b * (1.0 - ohms / 100.0))) / (2.0 * b)


def _slope(points: list[tuple[float, float]]) -> float:
    """Least-squares slope of (t, value) points, per second."""
    mean_t = sum(t for t, _ in points) / len(points)
    mean_v = sum(v for _, v in points) / len(points)
    spread = sum((t - mean_t) ** 2 for t, _ in points)
    if spread <= 0.0:
        return 0.0
    return sum((t - mean_t) * (v - mean_v) for t, v in points) / spread


def heaters_on(duties: list[float]) -> str:
    return ", ".join(f"H{i} {duty:g}" for i, duty in enumerate(duties) if duty > 0.0) or "-"


def describe_terminal(terminal: Optional[Terminal]) -> str:
    if terminal is None:
        return "no reading"
    if not terminal.conducting:
        return f"{terminal.fault} {terminal.ohms:.1f} ohm (no probe)"
    return f"{terminal.fault} {terminal.ohms:.1f} ohm {pt100_c(terminal.ohms):.1f} C"


class Onboard:
    """One command per connection, like every other client of port 5000."""

    def __init__(self, host: str, port: int,
                 send: Callable[[str, str, int], str] = hardware_setup.send_command) -> None:
        self.host = host
        self.port = port
        self._send = send

    def raw(self, command: str) -> str:
        return self._send(command, self.host, self.port)

    def ack(self, command: str) -> dict[str, str]:
        return reply_fields(command, self.raw(command))

    def terminals(self) -> Optional[list[Terminal]]:
        """The RTD card terminals as COMPONENTS lists them; None when the
        service does not answer."""
        try:
            return parse_terminals(self.ack("COMPONENTS").get("sequent_rtd_ch", ""))
        except (OSError, AssociationError):
            return None


def poll(onboard: Onboard, heater_count: int) -> tuple[dict[int, Terminal], list[float]]:
    """One COMPONENTS + GET_THERMAL: ({terminal: reading}, applied duty per
    heater). Raises when the card has stopped answering."""
    components = onboard.ack("COMPONENTS")
    thermal = onboard.ack("GET_THERMAL")
    rtd_error = components.get("sequent_rtd_error", "")
    if rtd_error not in FRESH_RTD_ERRORS:
        raise AssociationError(f"RTD card not reading (sequent_rtd_error={rtd_error or '?'})")
    terminals: dict[int, Terminal] = {}
    for terminal in parse_terminals(components.get("sequent_rtd_ch", "")):
        terminals.setdefault(terminal.channel, terminal)
    duties = []
    for index in range(heater_count):
        try:
            duties.append(float(thermal.get(f"h{index}_duty", "nan")))
        except ValueError:
            duties.append(math.nan)
    return terminals, duties


class Survey:
    """Heats heaters one at a time and finds the terminal each one warms."""

    def __init__(self, onboard: Onboard, settings: Settings,
                 heater_lines: list[int], channels: set[int], *,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep,
                 out: Callable[[str], None] = print,
                 log_path: Optional[Path] = None,
                 verbose: bool = False) -> None:
        self.onboard = onboard
        self.s = settings
        self.heater_lines = heater_lines
        self.channels = sorted(channels)
        self.clock = clock
        self.sleep = sleep
        self.out = out
        self.verbose = verbose
        self.t0 = clock()
        self.readings: list[Reading] = []
        self.base: dict[int, float] = {}
        self._recent = {c: deque(maxlen=3) for c in self.channels}
        self._dropouts = dict.fromkeys(self.channels, 0)
        self._failed_polls = 0
        self._log_file = None
        self._log = None
        if log_path is not None:
            try:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                self._log_file = log_path.open("w", newline="", encoding="utf-8")
            except OSError as error:
                out(f"(not logging readings: {error})")
            else:
                self._log = csv.writer(self._log_file)
                self._log.writerow(
                    ["t_s", "phase", "heater"]
                    + [f"ch{c}_ohm" for c in CARD_TERMINALS]
                    + [f"h{i}_duty" for i in range(len(heater_lines))])

    def close(self) -> None:
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    def say(self, message: str) -> None:
        self.out(f"[{self.clock() - self.t0:5.0f}s] {message}")

    def run(self) -> list[HeaterResult]:
        self.onboard.ack("HEATERS_OFF")
        self.say("heaters off (any operator targets cleared)")
        results = []
        for heater in range(len(self.heater_lines)):
            self.wait_steady(f"before H{heater}")
            results.append(self.test(heater))
        return results

    def read(self, phase: str, heater: Optional[int] = None) -> Optional[Reading]:
        """One poll. None when it failed; three failures in a row raise, and
        so do a probe that stops reading and a terminal at abort_c."""
        self.sleep(self.s.poll_s)
        try:
            by_channel, duties = poll(self.onboard, len(self.heater_lines))
        except (OSError, AssociationError) as error:
            self._failed_polls += 1
            if self._failed_polls >= 3:
                raise AssociationError(f"no reading for 3 polls: {error}") from error
            return None
        self._failed_polls = 0

        raw: dict[int, float] = {}
        temps: dict[int, float] = {}
        for channel in self.channels:
            terminal = by_channel.get(channel)
            if terminal is not None and terminal.conducting:
                raw[channel] = pt100_c(terminal.ohms)
                self._recent[channel].append(raw[channel])
                temps[channel] = statistics.median(self._recent[channel])
                self._dropouts[channel] = 0
                continue
            # A probe lost mid-run is lost feedback, maybe on the specimen
            # being heated: stop rather than carry on half-blind.
            self._dropouts[channel] += 1
            if self._dropouts[channel] >= 3:
                raise AssociationError(
                    f"the PT100 on ch{channel} stopped reading ({describe_terminal(terminal)}); "
                    "fix that terminal and rerun")

        reading = Reading(self.clock(), raw, temps, duties)
        self.readings.append(reading)
        if self._log is not None:
            self._log.writerow(
                [f"{reading.t - self.t0:.1f}", phase, "" if heater is None else heater]
                + [f"{by_channel[c].ohms:.1f}" if c in by_channel else ""
                   for c in CARD_TERMINALS]
                + [f"{d:g}" for d in duties])

        hot = {c: v for c, v in temps.items() if v >= self.s.abort_c}
        if hot:
            channel = max(hot, key=hot.get)
            message = (f"ch{channel} reads {hot[channel]:.1f} C, at or above the "
                       f"{self.s.abort_c:g} C abort limit")
            earlier = next((r for r in reversed(self.readings[:-1])
                            if channel in r.temps), None)
            if earlier is not None and reading.t > earlier.t:
                rate = (hot[channel] - earlier.temps[channel]) / (reading.t - earlier.t)
                if rate > 2.0:
                    message += (f" (+{rate:.0f} C/s: a loose PT100 terminal "
                                "looks like this, real heat does not)")
            raise AssociationError(message)
        return reading

    def warming(self, since: float) -> dict[int, float]:
        """Terminals whose reads since `since` rose faster than
        steady_c_per_s: {terminal: C/s}. Fitted on raw reads -- read-to-read
        scatter averages out of a slope, the median filter would make steps."""
        window = [r for r in self.readings if r.t >= since]
        rates = {}
        for channel in self.channels:
            points = [(r.t, r.raw[channel]) for r in window if channel in r.raw]
            if len(points) >= 5:
                rate = _slope(points)
                if rate > self.s.steady_c_per_s:
                    rates[channel] = rate
        return rates

    def wait_steady(self, label: str) -> None:
        """Poll until no terminal is warming any more (at most max_wait_s)."""
        self.say(f"{label}: waiting until no PT100 is warming "
                 f"(at least {self.s.steady_s:.0f} s)")
        start = last_report = self.clock()
        driven = 0
        while True:
            reading = self.read("wait")
            now = self.clock()
            if reading is not None:
                on = [i for i, duty in enumerate(reading.duties) if duty > 0.0]
                driven = driven + 1 if on and now - start >= self.s.duty_grace_s else 0
                if driven >= 2:
                    raise AssociationError(
                        f"H{on[0]} is driven (duty {reading.duties[on[0]]:g}) although "
                        "this script switched the heaters off: something else is "
                        "commanding heaters")
            if now - start < self.s.steady_s or not self.readings:
                continue
            warming = self.warming(now - self.s.steady_s)
            if not warming:
                return
            listed = ", ".join(f"ch{c} {rate * 60.0:+.1f} C/min"
                               for c, rate in sorted(warming.items()))
            if now - start >= self.s.max_wait_s:
                self.say(f"{label}: still warming after {self.s.max_wait_s:.0f} s "
                         f"({listed}); carrying on")
                return
            if now - last_report >= 10.0:
                self.say(f"{label}: still warming: {listed}")
                last_report = now

    def baseline(self) -> dict[int, float]:
        """Each terminal's mean over its last three reads."""
        base = {}
        for channel in self.channels:
            values = [r.raw[channel] for r in self.readings[-6:] if channel in r.raw][-3:]
            if values:
                base[channel] = sum(values) / len(values)
        return base

    def table_header(self) -> str:
        return ("        " + "".join(f"ch{c}".rjust(6) for c in CARD_TERMINALS)
                + "  heaters on   (C above the reading at switch-on)")

    def table_row(self, seconds: float, reading: Reading) -> str:
        cells = "".join(f"{reading.temps[c] - self.base[c]:+6.1f}"
                        if c in reading.temps and c in self.base else "-".rjust(6)
                        for c in CARD_TERMINALS)
        return f"  {seconds:5.0f}s{cells}  {heaters_on(reading.duties)}"

    def test(self, heater: int) -> HeaterResult:
        """Heat one heater until one terminal clearly warms, two warm alike,
        or max_heat_s passes; the heater is off again on return."""
        line = self.heater_lines[heater]
        self.base = self.baseline()
        # repr, not :g -- a rounded-up duty would exceed heater.debug_max_duty.
        command = f"HEATER_TEST {heater} {self.s.duty!r} {self.s.pulse_s!r}"
        refresh_s = max(self.s.poll_s, self.s.pulse_s / 2.5)
        self.say(f"H{heater} (BCM {line}) on at duty {self.s.duty:g}")
        if self.verbose:
            self.out(self.table_header())
        start = self.clock()
        self._pulse(command)
        last_pulse = last_report = start
        pulse_failures = undriven = others_on = confirmed = 0
        ranked: list[tuple[int, float]] = []
        verdict = None
        elapsed = 0.0
        while verdict is None:
            reading = self.read("heat", heater)
            now = self.clock()
            elapsed = now - start
            if now - last_pulse >= refresh_s:
                try:
                    self._pulse(command)
                except OSError as error:
                    pulse_failures += 1
                    if pulse_failures >= 3:
                        raise AssociationError(
                            f"{command} not reaching the onboard: {error}") from error
                else:
                    last_pulse = now
                    pulse_failures = 0
            if reading is not None:
                if elapsed >= self.s.duty_grace_s:
                    # Check the onboard really drives this heater and only
                    # this one, or a silent heater would be blamed on wiring.
                    undriven = 0 if reading.duties[heater] > 0.0 else undriven + 1
                    on = [i for i, d in enumerate(reading.duties) if d > 0.0 and i != heater]
                    others_on = others_on + 1 if on else 0
                    if others_on >= 2:
                        raise AssociationError(
                            f"H{on[0]} came on while H{heater} was under test: "
                            "something else is commanding heaters")
                ranked = sorted(((c, reading.temps[c] - level) for c, level in self.base.items()
                                 if c in reading.temps),
                                key=lambda item: item[1], reverse=True)
                best = ranked[0][1] if ranked else 0.0
                second = ranked[1][1] if len(ranked) > 1 else 0.0
                clear = (best >= self.s.rise_c
                         and second <= max(self.s.noise_c, best / self.s.dominance))
                confirmed = confirmed + 1 if clear else 0
                if self.verbose:
                    self.out(self.table_row(elapsed, reading))
                if confirmed >= 2:
                    verdict = "paired"
                elif undriven >= 3:
                    verdict = "not driven"
                elif not clear and best >= 2.0 * self.s.rise_c:
                    # Two terminals warming together do not separate with more heat.
                    verdict = "ambiguous"
                elif not self.verbose and now - last_report >= 5.0:
                    self.say(f"H{heater}: {elapsed:.0f} s, "
                             + ", ".join(f"ch{c} {v:+.1f} C" for c, v in ranked[:3]))
                    last_report = now
            if verdict is None and elapsed >= self.s.max_heat_s:
                verdict = "no response"
        self.onboard.ack("HEATERS_OFF")
        channel, rise = ranked[0] if ranked else (None, 0.0)
        result = HeaterResult(heater, line, verdict, channel, rise,
                              ranked[1] if len(ranked) > 1 else None, elapsed)
        self.say(describe(result) + "; heater off")
        return result

    def observe(self, seconds: float) -> None:
        """Keep printing every terminal against the last switch-on reading,
        heaters off: a PT100 goes on rising after its heater stops."""
        start = self.clock()
        while self.clock() - start < seconds:
            reading = self.read("after")
            if reading is not None:
                self.out(self.table_row(self.clock() - start, reading) + "  (off)")

    def _pulse(self, command: str) -> None:
        reply = self.onboard.raw(command)
        if not reply.startswith("ACK"):
            raise AssociationError(f"{command} -> {reply or 'no reply'}")


def describe(result: HeaterResult) -> str:
    head = f"H{result.heater} (BCM {result.line})"
    runner = (f", next ch{result.runner_up[0]} {result.runner_up[1]:+.1f} C"
              if result.runner_up else "")
    if result.verdict == "paired":
        return (f"{head} -> ch{result.channel}: {result.rise_c:+.1f} C after "
                f"{result.seconds:.0f} s{runner}")
    if result.verdict == "not driven":
        return f"{head}: not driven (the onboard applied duty 0)"
    warmest = (f"warmest ch{result.channel} {result.rise_c:+.1f} C"
               if result.channel is not None else "no reading")
    return f"{head}: {result.verdict} after {result.seconds:.0f} s ({warmest}{runner})"


def resolve(results: list[HeaterResult]) -> tuple[dict[int, int], dict[int, str]]:
    """({heater: terminal} for the pairs to write, {heater: why not} for the rest)."""
    problems: dict[int, str] = {}
    for result in results:
        if result.verdict == "no response":
            problems[result.heater] = (
                f"no terminal warmed in {result.seconds:.0f} s. The heater is not on "
                f"BCM {result.line}, not connected, or its specimen's PT100 is not on "
                "the card")
        elif result.verdict == "ambiguous":
            other = (f"ch{result.runner_up[0]} {result.runner_up[1]:+.1f} C"
                     if result.runner_up else "another terminal")
            problems[result.heater] = (
                f"ch{result.channel} {result.rise_c:+.1f} C and {other} warmed "
                "together. A heater touching two specimens, or a PT100 off its "
                "specimen (the neighbours then lead)")
        elif result.verdict == "not driven":
            problems[result.heater] = (
                "the onboard accepted HEATER_TEST but applied duty 0 (heater energy "
                "budget latch? its feedback PT100 invalid?)")
    paired = [r for r in results if r.verdict == "paired"]
    heaters_by_channel: dict[int, list[int]] = {}
    for result in paired:
        heaters_by_channel.setdefault(result.channel, []).append(result.heater)
    for channel, heaters in heaters_by_channel.items():
        if len(heaters) > 1:
            names = " and ".join(f"H{h}" for h in heaters)
            for heater in heaters:
                problems[heater] = (f"{names} both warmed ch{channel}: two heater "
                                    "outputs on one specimen")
    # A PT100 off its specimen leaves the specimen next door as the warmest
    # terminal, and conducted heat arrives far slower than direct heat. The
    # heaters and specimens are alike (bench 2026-09-14: +1 C within 2-4 s
    # for every heater), so a heater far slower than the rest is not trusted.
    # The +10 s keeps poll jitter from flagging anything on a fast rig.
    times = [r.seconds for r in paired if r.heater not in problems]
    if len(times) >= 3:
        typical = statistics.median(times)
        for result in paired:
            if (result.heater not in problems
                    and result.seconds > max(3.0 * typical, typical + 10.0)):
                problems[result.heater] = (
                    f"took {result.seconds:.0f} s to warm ch{result.channel}, the "
                    f"others {typical:.0f} s. A PT100 off its own specimen reads the "
                    "neighbouring specimen's heat like this")
    association = {r.heater: r.channel for r in paired if r.heater not in problems}
    return association, problems


def compose_channel_map(association: dict[int, int], current_map: list[int],
                        sample_count: int) -> list[int]:
    """sensor.sequent_rtd_channels with S<h> read from the terminal heater h
    warmed. Every other sample keeps its current terminal unless a paired
    heater took it; those samples get the terminals left over, in the order
    the current map lists them (heat cannot rank them)."""
    new_map: list[Optional[int]] = [association.get(s) for s in range(sample_count)]
    taken = set(association.values())
    for sample in range(sample_count):
        if (new_map[sample] is None and sample < len(current_map)
                and current_map[sample] not in taken):
            new_map[sample] = current_map[sample]
            taken.add(current_map[sample])
    spare = [c for c in dict.fromkeys([*current_map, *CARD_TERMINALS]) if c not in taken]
    return [channel if channel is not None else spare.pop(0) for channel in new_map]


def load_mapping(values: dict[str, str]) -> Mapping:
    """The wiring in an INI's values. KeyError or ValueError when unreadable."""
    numbers = hardware_setup._number_list
    heater_lines = numbers(values["heater.output_lines"])
    sample_count = int(values.get("hardware.sample_count", "8"))
    return Mapping(
        heater_lines=heater_lines,
        channels=numbers(values.get("sensor.sequent_rtd_channels",
                                    ",".join(str(c) for c in range(1, sample_count + 1)))),
        motors=[numbers(values.get(f"motor{m}.samples",
                                   ",".join(str(s) for s in GS_MOTOR_SAMPLES[m])))
                for m in range(MOTOR_COUNT)],
        clicks=numbers(values.get("sensor.max31865_sample_indices",
                                  ",".join(str(s) for s in GS_CLICK_SAMPLES))),
        temperature_channels=numbers(values.get(
            "heater.temperature_channels", ",".join(str(i) for i in range(len(heater_lines))))),
    )


def mapping_rows(mapping: Mapping, terminals: Optional[dict[int, Terminal]] = None,
                 notes: Optional[dict[int, str]] = None) -> list[str]:
    """A table, one line per sample: motor, heater, PT100 terminal, MAX31865
    click, and when given the live reading and a note."""
    rows = ["  Sample  Motor  Heater        PT100  Click"
            + ("  Now" if terminals is not None else "")]
    for sample, channel in enumerate(mapping.channels):
        motor = mapping.motor_of(sample)
        motor_text = "-" if motor is None else f"M{motor}"
        heater = (f"H{sample} BCM {mapping.heater_lines[sample]}"
                  if sample < mapping.heater_count else "unheated")
        click = str(mapping.clicks.index(sample) + 1) if sample in mapping.clicks else ""
        row = f"  S{sample:<6} {motor_text:<6} {heater:<13} ch{channel:<4} {click:<5}"
        if terminals is not None:
            row += f"  {describe_terminal(terminals.get(channel))}"
        if notes and sample in notes:
            row += f"  {notes[sample]}"
        rows.append(row.rstrip())
    return rows


def assign_command(mapping: Mapping) -> str:
    """The assign command that writes `mapping` (heated samples first in each group)."""
    groups = []
    for motor, samples in enumerate(mapping.motors):
        entries = [f"ch{mapping.channels[s]}"
                   + (f":{mapping.heater_lines[s]}" if s < mapping.heater_count else "")
                   for s in sorted(samples) if s < len(mapping.channels)]
        groups.append(f"--motor{motor} {','.join(entries)}")
    return f"{SCRIPT} assign {' '.join(groups)}"


def mapping_warnings(mapping: Mapping) -> list[str]:
    warnings = []
    identity = list(range(mapping.heater_count))
    if mapping.temperature_channels != identity:
        warnings.append(
            f"heater.temperature_channels={','.join(map(str, mapping.temperature_channels))}: "
            "heater i must read sample i (the ground station assumes it, and "
            "coatheal-deploy resets it to 0..5)")
    covered = sorted(s for samples in mapping.motors for s in samples)
    if covered != list(range(len(mapping.channels))):
        warnings.append("motor0.samples and motor1.samples must list every sample once")
    elif [sorted(s) for s in mapping.motors] != [list(g) for g in GS_MOTOR_SAMPLES]:
        warnings.append("the motor groups are not the ground station's (M0 = S0-S3, "
                        "M1 = S4-S7): its motion and bend panels name the wrong samples")
    if tuple(mapping.clicks) != GS_CLICK_SAMPLES:
        warnings.append("sensor.max31865_sample_indices is not the ground station's (S0, S4)")
    return warnings


def parse_group(text: str) -> list[Specimen]:
    """`ch8:19,ch2:13,ch1` -> specimens, in the order given."""
    specimens = []
    for item in text.split(","):
        if not item.strip():
            continue
        terminal, sep, heater = (part.strip().lower() for part in item.partition(":"))
        heater = heater.removeprefix("bcm")
        if not (terminal.startswith("ch") and terminal[2:].isdigit()) or (
                sep and not heater.isdigit()):
            raise ValueError(f"{item.strip()!r}: give a PT100 terminal and its heater's "
                             "BCM line, like ch8:19, or the terminal alone for an "
                             "unheated specimen")
        specimens.append(Specimen(int(terminal[2:]), int(heater) if sep else None))
    return specimens


def plan_assignment(groups: list[list[Specimen]], mapping: Mapping) -> Mapping:
    """The mapping that puts each motor's specimens on that motor's samples:
    heated ones on the samples that have a heater (S<i>, i < heater count)
    in the order given, unheated ones on the rest. ValueError says what does
    not fit."""
    specimens = [s for group in groups for s in group]
    sample_count = len(mapping.channels)
    problems = []
    channels = [s.channel for s in specimens]
    lines = [s.line for s in specimens if s.line is not None]
    if len(specimens) != sample_count:
        problems.append(f"{len(specimens)} specimens given, the config has {sample_count} samples")
    repeated = sorted({c for c in channels if channels.count(c) > 1})
    if repeated:
        problems.append(", ".join(f"ch{c}" for c in repeated) + " given twice")
    outside = sorted({c for c in channels if c not in CARD_TERMINALS})
    if outside:
        problems.append(", ".join(f"ch{c}" for c in outside)
                        + " is not a card terminal (ch1-ch8)")
    repeated_lines = sorted({line for line in lines if lines.count(line) > 1})
    if repeated_lines:
        problems.append(", ".join(f"BCM {line}" for line in repeated_lines)
                        + " heats two specimens")
    if sorted(s for samples in mapping.motors for s in samples) != list(range(sample_count)):
        problems.append("motor0.samples and motor1.samples in the config must list every "
                        "sample once")
    if problems:
        raise ValueError("; ".join(problems))

    heater_lines = list(mapping.heater_lines)
    terminal_map = list(mapping.channels)
    for motor, group in enumerate(groups):
        slots = sorted(mapping.motors[motor])
        heated_slots = [s for s in slots if s < mapping.heater_count]
        unheated_slots = [s for s in slots if s >= mapping.heater_count]
        heated = [s for s in group if s.line is not None]
        unheated = [s for s in group if s.line is None]
        if len(heated) != len(heated_slots) or len(unheated) != len(unheated_slots):
            names = ", ".join(f"S{s}" for s in slots)
            problems.append(f"motor {motor} pulls {len(heated_slots)} heated and "
                            f"{len(unheated_slots)} unheated specimens ({names}); "
                            f"{len(heated)} heated and {len(unheated)} unheated given")
            continue
        for slot, specimen in zip(heated_slots, heated):
            heater_lines[slot] = specimen.line
            terminal_map[slot] = specimen.channel
        for slot, specimen in zip(unheated_slots, unheated):
            terminal_map[slot] = specimen.channel
    if problems:
        raise ValueError("; ".join(problems))
    return replace(mapping, heater_lines=heater_lines, channels=terminal_map,
                   temperature_channels=list(range(mapping.heater_count)))


def check_ready(onboard: Onboard, heater_count: int, sample_count: int, abort_c: float,
                require_all_probes: bool = True, out: Callable[[str], None] = print
                ) -> tuple[dict[str, str], list[Terminal], list[str]]:
    """Report what the running onboard sees; return (STATUS, terminals, blockers)."""
    try:
        status = onboard.ack("STATUS")
        components = onboard.ack("COMPONENTS")
        thermal = onboard.ack("GET_THERMAL")
    except OSError as error:
        raise AssociationError(
            f"cannot reach the onboard command port ({error}); is {SERVICE} running?") from error
    blockers = []
    out("Onboard: " + " ".join(
        f"{key}={status.get(key, '?')}"
        for key in ("mode", "phase", "bench_mode", "debug_armed", "simulated")))
    if status.get("bench_mode") != "1":
        blockers.append("runtime.bench_mode is off, and HEATER_TEST needs it: set "
                        "runtime.bench_mode=true, restart the service, rerun (and set "
                        "it back before flight)")
    if status.get("simulated") == "1":
        blockers.append("the onboard runs simulated sensors")
    if status.get("mode") == "SAFE":
        blockers.append("the onboard is in SAFE mode (EXIT_SAFE first)")
    if status.get("silence") == "1":
        blockers.append("radio silence is active (RADIO_RESUME first)")
    if status.get("fallback_active") == "1":
        blockers.append("link-loss fallback is active (reconnect the ground station "
                        "or DISARM first)")
    if status.get("budget_exhausted") == "1":
        blockers.append("the heater energy budget latch is tripped (RESET_CTRL first)")

    running_heaters = sum(1 for key in thermal if key.startswith("h") and key.endswith("_duty"))
    if running_heaters != heater_count:
        blockers.append(f"the running onboard has {running_heaters} heaters but the config "
                        f"lists {heater_count}: restart the service on this config first")
    unclaimed = [f"H{i}" for i in range(heater_count)
                 if components.get(f"heater{i}") != "OK"]
    if unclaimed:
        blockers.append(f"heater output {', '.join(unclaimed)} not available "
                        "(heaterN=FAILED in COMPONENTS: GPIO line not claimed)")

    terminals: list[Terminal] = []
    seen: set[int] = set()
    for terminal in parse_terminals(components.get("sequent_rtd_ch", "")):
        if terminal.channel not in seen:
            seen.add(terminal.channel)
            terminals.append(terminal)
    rtd_error = components.get("sequent_rtd_error", "")
    if not terminals or rtd_error not in FRESH_RTD_ERRORS:
        blockers.append(f"no RTD card reading (sequent_rtd={components.get('sequent_rtd', '?')}, "
                        f"sequent_rtd_error={rtd_error or '?'})")
    else:
        out(f"RTD card terminals (sequent_rtd={components.get('sequent_rtd', '?')}):")
        for terminal in sorted(terminals, key=lambda t: t.channel):
            out(f"  ch{terminal.channel} -> S{terminal.sample}  {describe_terminal(terminal)}")
        usable = [t for t in terminals if t.conducting]
        silent = ", ".join(f"ch{t.channel}" for t in terminals if not t.conducting)
        if len(usable) < sample_count:
            if require_all_probes:
                # With a heater's own probe missing, the PT100 of the specimen
                # next to it warms by conduction and would be paired instead.
                blockers.append(
                    f"{silent or 'some terminals'} read no PT100: every one of the "
                    f"{sample_count} probes must read before pairing, or a neighbouring "
                    "specimen's probe can pass for a heater's own")
            else:
                out(f"  ({silent or 'some terminals'} read no PT100 and are not watched; "
                    "a heater whose own probe is missing warms nothing here)")
        hot = [t for t in usable if pt100_c(t.ohms) >= abort_c]
        if hot:
            blockers.append("already at or above the abort limit: "
                            + ", ".join(f"ch{t.channel} {pt100_c(t.ohms):.1f} C" for t in hot))
    return status, terminals, blockers


def config_changes(values: dict[str, str], mapping: Mapping) -> dict[str, str]:
    """The INI keys whose value differs from `mapping`."""
    wanted = {
        "heater.output_lines": mapping.heater_lines,
        "sensor.sequent_rtd_channels": mapping.channels,
        # Heater i reads sample i; the wiring lives in the two maps above.
        "heater.temperature_channels": list(range(mapping.heater_count)),
    }
    changes = {}
    for key, numbers in wanted.items():
        try:
            current = hardware_setup._number_list(values.get(key, ""))
        except ValueError:
            current = None
        if current != numbers:
            changes[key] = ",".join(str(n) for n in numbers)
    return changes


def _write_ini(path: Path, text: str) -> None:
    owner = path.stat()
    hardware_setup.atomic_write(path, text)
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        os.chown(path, owner.st_uid, owner.st_gid)  # keep it editable by coatheal after sudo


def write_config(path: Path, changes: dict[str, str], yes: bool,
                 ask: Callable[[str], str] = input,
                 out: Callable[[str], None] = print) -> bool:
    """Apply `changes` to the INI, backup first. False when nothing was
    written because the validator refused it or the operator said no."""
    text = path.read_text(encoding="utf-8")
    values = hardware_setup._ini_values(text)
    candidate = hardware_setup.replace_ini(text, changes)
    errors = hardware_setup.validate_candidate(candidate)
    for error in errors:
        out(f"  configuration error: {error}")
    if errors or hardware_setup._check_with_binary(candidate) != 0:
        out("The assignment was refused (above), nothing written.")
        return False
    for key, value in changes.items():
        out(f"  {key}: {values.get(key, '(unset)')} -> {value}")
    if not yes and ask(f"Write this to {path}? [y/N]: ").strip().lower() not in ("y", "yes"):
        out("No files changed.")
        return False
    backup = hardware_setup._backup_path(path)
    shutil.copy2(path, backup)
    _write_ini(path, candidate)
    out(f"Backed up {path} -> {backup}")
    out(f"Wrote {path}")
    return True


def restart_and_verify(onboard: Onboard, mapping: Mapping,
                       clock: Callable[[], float] = time.monotonic,
                       sleep: Callable[[float], None] = time.sleep,
                       out: Callable[[str], None] = print) -> bool:
    command = ["systemctl", "restart", SERVICE]
    if os.geteuid() != 0:
        command = ["sudo", "-n", *command]
    out(f"Restarting: {' '.join(command)}")
    if subprocess.run(command, check=False).returncode != 0:
        out(f"Restart failed; run it by hand: sudo systemctl restart {SERVICE}")
        return False
    deadline = clock() + 60.0
    components: dict[str, str] = {}
    terminals: list[Terminal] = []
    while not terminals and clock() < deadline:
        sleep(2.0)
        try:
            components = onboard.ack("COMPONENTS")
            terminals = parse_terminals(components.get("sequent_rtd_ch", ""))
        except (OSError, AssociationError):
            terminals = []
    if not terminals:
        out(f"The service did not report RTD terminals within 60 s; see "
            f"journalctl -u {SERVICE}")
        return False
    running = [t.channel for t in sorted(terminals, key=lambda t: t.sample)][:len(mapping.channels)]
    if running != mapping.channels:
        out(f"MISMATCH: the restarted service reads terminals {running}, "
            f"expected {mapping.channels}")
        return False
    unclaimed = [f"H{i} (BCM {line})" for i, line in enumerate(mapping.heater_lines)
                 if components.get(f"heater{i}") != "OK"]
    if unclaimed:
        out(f"The restarted service did not claim {', '.join(unclaimed)}: that line is "
            f"not free (journalctl -u {SERVICE})")
        return False
    out("Service restarted and runs the new assignment: "
        + " ".join(f"S{i}=ch{c}" for i, c in enumerate(running)))
    return True


def restore(onboard: Onboard, armed_debug: bool, armed_run: bool,
            sleep: Callable[[float], None] = time.sleep) -> bool:
    """Heaters off, then undo only the arming this script did."""
    confirmed = False
    for _ in range(3):
        try:
            if onboard.raw("HEATERS_OFF").startswith("ACK"):
                confirmed = True
                break
        except OSError:
            pass
        sleep(1.0)
    for needed, command in ((armed_debug, "DISARM_DEBUG"), (armed_run, "DISARM")):
        if needed:
            try:
                onboard.raw(command)
            except OSError:
                pass
    return confirmed


def service_config() -> Optional[Path]:
    try:
        text = ENV_FILE.read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() == "COATHEAL_CONFIG":
            return Path(value.strip().strip('"'))
    return None


@dataclass
class Setup:
    """What every command works from: the config, and the service to talk to."""
    config_path: Path
    service_path: Optional[Path]
    values: dict[str, str]
    mapping: Mapping
    onboard: Onboard
    clock: Callable[[], float]
    sleep: Callable[[float], None]
    ask: Callable[[str], str]
    duty_cap: float
    pulse_cap: float

    @property
    def runs_this_config(self) -> bool:
        return (self.service_path is None
                or self.service_path.resolve() == self.config_path.resolve())

    @property
    def token(self) -> str:
        return self.values.get("runtime.debug_arm_code", "COATHEAL_DEBUG")

    def print_config(self) -> None:
        print(f"Config: {self.config_path}"
              + ("" if self.runs_this_config else f"  (the service runs {self.service_path})"))

    def settings(self, args: argparse.Namespace, max_heat_s: float) -> Settings:
        return Settings(duty=min(args.duty, self.duty_cap), rise_c=args.rise_c,
                        max_heat_s=max_heat_s, abort_c=args.abort_c,
                        pulse_s=min(Settings.pulse_s, self.pulse_cap))

    def preflight(self, settings: Settings, require_all_probes: bool
                  ) -> Optional[tuple[dict[str, str], list[Terminal]]]:
        """(STATUS, terminals) when heating may start, else None (reasons printed)."""
        if settings.duty <= 0.0 or settings.rise_c <= 0.0 or settings.max_heat_s <= 0.0:
            print("Need a duty, a rise and a heating time above 0.", file=sys.stderr)
            return None
        try:
            status, terminals, blockers = check_ready(
                self.onboard, self.mapping.heater_count, len(self.mapping.channels),
                settings.abort_c, require_all_probes)
        except AssociationError as error:
            status, terminals, blockers = {}, [], [str(error)]
        if blockers:
            print("\nNOT READY:")
            for blocker in blockers:
                print(f"  - {blocker}")
            return None
        try:
            tick_hz = float(status.get("tick_hz", "1"))
        except ValueError:
            tick_hz = 1.0
        if tick_hz > 0.0:
            settings.duty_grace_s = max(settings.duty_grace_s, 3.0 / tick_hz)
        return status, terminals


def load_setup(args: argparse.Namespace, send: Callable[[str, str, int], str],
               clock: Callable[[], float], sleep: Callable[[float], None],
               ask: Callable[[str], str]) -> Optional[Setup]:
    service_path = service_config()
    config_path: Path = args.config or service_path or hardware_setup.DEFAULT_CONFIG
    try:
        values = hardware_setup._ini_values(config_path.read_text(encoding="utf-8"))
        mapping = load_mapping(values)
        duty_cap = min(float(values.get("heater.debug_max_duty", "0.25")),
                       float(values.get("heater.max_duty", "1.0")))
        pulse_cap = float(values.get("heater.debug_max_seconds", "10"))
    except (OSError, KeyError, ValueError) as error:
        print(f"Cannot use {config_path}: {error}", file=sys.stderr)
        return None
    return Setup(config_path, service_path, values, mapping,
                 Onboard(args.host, args.port, send), clock, sleep, ask, duty_cap, pulse_cap)


def heating_session(setup: Setup, status: dict[str, str], work: Callable[[], object]
                    ) -> tuple[object, Optional[str]]:
    """Arm what heating needs, run work(), then switch the heaters off and
    undo the arming, whatever happened: (work's result, why it stopped or None)."""
    def stop(signum, frame):
        raise KeyboardInterrupt

    handlers = {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGHUP)}
    armed_debug = armed_run = False
    result: object = None
    failure = None
    try:
        if status.get("debug_armed") != "1":
            setup.onboard.ack(f"ARM_DEBUG {setup.token}")
            armed_debug = True
        if status.get("mode") == "STANDBY":
            setup.onboard.ack("ARM")
            armed_run = True
        result = work()
    except AssociationError as error:
        failure = str(error)
    except OSError as error:
        failure = f"lost the onboard ({error})"
    except KeyboardInterrupt:
        failure = "interrupted"
    finally:
        heaters_off = restore(setup.onboard, armed_debug, armed_run, setup.sleep)
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    if not heaters_off:
        print("\nWARNING: HEATERS_OFF was not confirmed. The last HEATER_TEST pulse "
              "lapses by itself within seconds; check the heaters.")
    return result, failure


def write_and_load(setup: Setup, mapping: Mapping, args: argparse.Namespace) -> int:
    """Write `mapping` into the config and restart the service onto it.
    0: written (or already there), 1: not written, 2: the restarted service
    does not run it."""
    changes = config_changes(setup.values, mapping)
    if not changes:
        print(f"{setup.config_path} already carries this assignment.")
    elif not write_config(setup.config_path, changes, args.yes, setup.ask):
        return 1
    dropped = sorted(set(setup.mapping.heater_lines) - set(mapping.heater_lines))
    added = sorted(set(mapping.heater_lines) - set(setup.mapping.heater_lines))
    if changes and (dropped or added):
        print(f"Heater lines changed ({', '.join(f'BCM {n}' for n in dropped)} out, "
              f"{', '.join(f'BCM {n}' for n in added)} in): run coatheal-deploy afterwards "
              "so config.txt holds the new lines off from boot (it will say REBOOT "
              "REQUIRED). Nothing holds a line that left the list: make sure no heater "
              "is wired to it.")
    if not setup.runs_this_config:
        print(f"\nThe service loads {setup.service_path}, not {setup.config_path}: copy the "
              f"change there (or repoint COATHEAL_CONFIG), then restart {SERVICE}.")
        return 0
    terminals = setup.onboard.terminals()
    running = ([t.channel for t in sorted(terminals, key=lambda t: t.sample)]
               if terminals else None)
    if not changes and running == mapping.channels:
        print("The running service already uses it.")
        return 0
    if args.no_restart:
        print(f"\nRestart the service to load it: sudo systemctl restart {SERVICE}")
        return 0
    if not args.yes and setup.ask("Restart the service now to load it? [y/N]: ") \
            .strip().lower() not in ("y", "yes"):
        print(f"Not restarted. Later: sudo systemctl restart {SERVICE}")
        return 0
    if not restart_and_verify(setup.onboard, mapping, setup.clock, setup.sleep):
        return 2
    print("Then prove one loop from the ground station: a small SET_TEMP_TARGET on H<i> "
          "must move S<i>, and only S<i>.")
    return 0


def log_path(args: argparse.Namespace, name: str) -> Path:
    return args.log or (hardware_setup.ROOT / "logs"
                        / f"{name}-{time.strftime('%Y%m%d-%H%M%S')}.csv")


def cmd_show(args: argparse.Namespace, setup: Setup) -> int:
    setup.print_config()
    mapping = setup.mapping
    terminals: Optional[dict[int, Terminal]] = None
    notes = mapping_warnings(mapping)
    try:
        status = setup.onboard.ack("STATUS")
        components = setup.onboard.ack("COMPONENTS")
    except (OSError, AssociationError) as error:
        print(f"(the onboard does not answer: {error}; the config alone follows)")
    else:
        print("Onboard: " + " ".join(f"{key}={status.get(key, '?')}"
                                     for key in ("mode", "phase", "bench_mode", "debug_armed")))
        listed = parse_terminals(components.get("sequent_rtd_ch", ""))
        terminals = {}
        for terminal in listed:
            terminals.setdefault(terminal.channel, terminal)
        running = [t.channel for t in sorted(listed, key=lambda t: t.sample)]
        if listed and running != mapping.channels:
            notes.append(f"the service reads terminals {','.join(map(str, running))}, not "
                         "the config's: restart it to load the config")
        unclaimed = [f"H{i}" for i in range(mapping.heater_count)
                     if components.get(f"heater{i}") != "OK"]
        if unclaimed:
            notes.append(f"{', '.join(unclaimed)} not claimed by the service (heaterN in "
                         "COMPONENTS): its line is taken or missing")
    print()
    for row in mapping_rows(mapping, terminals):
        print(row)
    for note in notes:
        print(f"  ! {note}")
    print("\nTo change it, edit and run:")
    print(f"  {assign_command(mapping)}")
    return 0


def cmd_watch(args: argparse.Namespace, setup: Setup) -> int:
    mapping = setup.mapping
    print("Every RTD card terminal once a second, C above its first reading "
          "(OPEN/SHORT: no probe). Heats and arms nothing. Ctrl+C stops.")
    start = setup.clock()
    first: Optional[dict[int, float]] = None
    try:
        while args.seconds is None or setup.clock() - start < args.seconds:
            setup.sleep(1.0)
            try:
                terminals, duties = poll(setup.onboard, mapping.heater_count)
            except (OSError, AssociationError) as error:
                print(f"  (no reading: {error})")
                continue
            temps = {c: pt100_c(t.ohms) for c, t in terminals.items() if t.conducting}
            if first is None:
                first = dict(temps)
                sample_of = {c: s for s, c in enumerate(mapping.channels)}
                print("        " + "".join(f"ch{c}".rjust(7) for c in CARD_TERMINALS)
                      + "  heaters on")
                print("  sample" + "".join((f"S{sample_of[c]}" if c in sample_of else "-")
                                           .rjust(7) for c in CARD_TERMINALS))
                print("   start" + "".join((f"{first[c]:.1f}" if c in first else "-")
                                           .rjust(7) for c in CARD_TERMINALS))
            cells = []
            for channel in CARD_TERMINALS:
                if channel in temps:
                    first.setdefault(channel, temps[channel])
                    cells.append(f"{temps[channel] - first[channel]:+7.1f}")
                elif channel in terminals:
                    cells.append(terminals[channel].fault.rjust(7))
                else:
                    cells.append("-".rjust(7))
            print(f"  {setup.clock() - start:5.0f}s{''.join(cells)}  {heaters_on(duties)}")
    except KeyboardInterrupt:
        pass
    return 0


def cmd_heat(args: argparse.Namespace, setup: Setup) -> int:
    mapping = setup.mapping
    name = args.heater.strip().upper().removeprefix("H")
    if not name.isdigit() or not 0 <= int(name) < mapping.heater_count:
        print(f"No heater {args.heater!r}: H0..H{mapping.heater_count - 1}.", file=sys.stderr)
        return 2
    heater = int(name)
    setup.print_config()
    settings = setup.settings(args, args.seconds)
    ready = setup.preflight(settings, require_all_probes=False)
    if ready is None:
        return 2
    status, terminals = ready
    line = mapping.heater_lines[heater]
    path = log_path(args, f"heater-test-H{heater}")
    survey = Survey(setup.onboard, settings, mapping.heater_lines,
                    {t.channel for t in terminals if t.conducting},
                    clock=setup.clock, sleep=setup.sleep, log_path=path, verbose=True)
    print(f"\nH{heater} (BCM {line}) alone, at most {settings.max_heat_s:.0f} s; nothing is "
          f"written. Ctrl+C stops safely. Readings: {path}")

    def work() -> HeaterResult:
        setup.onboard.ack("HEATERS_OFF")
        survey.wait_steady(f"before H{heater}")
        result = survey.test(heater)
        survey.observe(args.after_s)
        return result

    try:
        result, failure = heating_session(setup, status, work)
    finally:
        survey.close()
    if failure is not None:
        print(f"\nSTOPPED: {failure}")
        return 2
    if result.verdict != "paired":
        _, problems = resolve([result])
        print(f"\nH{heater} (BCM {line}) did not pair: {problems[heater]}.")
        return 1
    configured = mapping.channels[heater]
    if result.channel == configured:
        print(f"\nH{heater} (BCM {line}) warms ch{result.channel}, which the config reads "
              f"as S{heater}: they agree.")
    else:
        print(f"\nH{heater} (BCM {line}) warms ch{result.channel}, but the config reads "
              f"S{heater} from ch{configured}. `auto` measures and writes every pair; "
              "`assign` sets them by hand.")
    return 0


def cmd_auto(args: argparse.Namespace, setup: Setup) -> int:
    mapping = setup.mapping
    setup.print_config()
    settings = setup.settings(args, args.max_heat_s)
    ready = setup.preflight(settings, require_all_probes=True)
    if ready is None:
        return 2
    status, terminals = ready
    if args.check:
        print(f"\nReady: {mapping.heater_count} heaters, one at a time at duty "
              f"{settings.duty:g}, each until a PT100 warms by {settings.rise_c:g} C "
              f"(at most {settings.max_heat_s:.0f} s).")
        return 0

    path = log_path(args, "heater-association")
    survey = Survey(setup.onboard, settings, mapping.heater_lines,
                    {t.channel for t in terminals if t.conducting},
                    clock=setup.clock, sleep=setup.sleep, log_path=path, verbose=args.verbose)
    print(f"\nHeating {mapping.heater_count} heaters one at a time. Ctrl+C stops safely. "
          f"Readings: {path}")
    try:
        results, failure = heating_session(setup, status, survey.run)
    finally:
        survey.close()
    if failure is not None:
        print(f"\nSTOPPED: {failure}\nNothing was written.")
        return 2

    association, problems = resolve(results)
    print("\nPairs:")
    for result in results:
        head = f"  H{result.heater} (BCM {result.line})"
        if result.heater in association:
            print(f"{head} -> ch{association[result.heater]}")
        else:
            print(f"{head} -> NOT PAIRED: {problems[result.heater]}.")
    if not association:
        print("\nNo heater paired with a terminal, so nothing was written. Is the heater "
              f"supply on? Readings: {path}")
        return 1
    new = replace(mapping,
                  channels=compose_channel_map(association, mapping.channels,
                                               len(mapping.channels)),
                  temperature_channels=list(range(mapping.heater_count)))
    notes = {sample: "measured" if sample in association
             else f"NOT measured: do not heat H{sample} until a rerun pairs it"
             for sample in range(mapping.heater_count)}
    print("\nAssignment (S<i> = the specimen H<i> warms):")
    for row in mapping_rows(new, notes=notes):
        print(row)
    print("\nHeat cannot tell which motor pulls a specimen, which unheated terminal is\n"
          "S6 and which S7, or which specimen is on a MAX31865 click. Where the table\n"
          "is wrong about those, correct it and run:\n"
          f"  {assign_command(new)}")
    done = 0 if not problems else 1
    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return done
    if problems:
        left_out = ", ".join(f"H{h}" for h in sorted(problems))
        print(f"\n{left_out} left out: fix what is reported above, then rerun to pair "
              f"{'it' if len(problems) == 1 else 'them'}, or assign by hand.")
    print()
    return max(write_and_load(setup, new, args), done)


def cmd_assign(args: argparse.Namespace, setup: Setup) -> int:
    setup.print_config()
    old = setup.mapping
    try:
        new = plan_assignment([parse_group(args.motor0), parse_group(args.motor1)], old)
    except ValueError as error:
        print(f"\nCannot assign: {error}.", file=sys.stderr)
        return 2
    listed = setup.onboard.terminals()
    terminals = None if listed is None else {t.channel: t for t in listed}
    notes = {}
    for sample in range(len(new.channels)):
        was = []
        if sample < new.heater_count and new.heater_lines[sample] != old.heater_lines[sample]:
            was.append(f"heater was BCM {old.heater_lines[sample]}")
        if new.channels[sample] != old.channels[sample]:
            was.append(f"PT100 was ch{old.channels[sample]}")
        if was:
            notes[sample] = f"({', '.join(was)})"
    print("\nAssignment:")
    for row in mapping_rows(new, terminals, notes):
        print(row)
    for warning in mapping_warnings(new):
        print(f"  ! {warning}")
    if terminals is not None:
        silent = [f"ch{c}" for c in new.channels
                  if c not in terminals or not terminals[c].conducting]
        if silent:
            print(f"  ! {', '.join(silent)} read no PT100 right now")
    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return 0
    print()
    return write_and_load(setup, new, args)


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--config", type=Path, default=None,
                        help="INI to use (default: the service's COATHEAL_CONFIG, "
                             "else config/onboard.local.ini)")
    common.add_argument("--host", default="127.0.0.1")
    common.add_argument("--port", type=int, default=5000)

    defaults = Settings()
    heating = argparse.ArgumentParser(add_help=False)
    heating.add_argument("--duty", type=float, default=defaults.duty,
                         help="heater duty (capped by heater.debug_max_duty and "
                              "heater.max_duty; default %(default)s)")
    heating.add_argument("--rise-c", type=float, default=defaults.rise_c,
                         help="rise that pairs a terminal with the heater (default %(default)s)")
    heating.add_argument("--abort-c", type=float, default=defaults.abort_c,
                         help="any terminal reaching this stops (default %(default)s)")
    heating.add_argument("--log", type=Path, default=None,
                         help="CSV of every reading (default logs/<command>-<time>.csv)")

    writing = argparse.ArgumentParser(add_help=False)
    writing.add_argument("--dry-run", action="store_true", help="write nothing")
    writing.add_argument("--yes", action="store_true",
                         help="write the config and restart the service without asking")
    writing.add_argument("--no-restart", action="store_true",
                         help="write the config but leave the service as it runs")

    root = argparse.ArgumentParser(
        description="Heaters, PT100s and motor groups in the onboard config: show, "
                    "debug, measure or assign them. Without a command: auto.")
    commands = root.add_subparsers(dest="command", metavar="command")
    commands.add_parser("show", parents=[common],
                        help="the assignment in the config, live readings, and the "
                             "assign command that reproduces it")
    watch = commands.add_parser("watch", parents=[common],
                                help="every PT100 once a second (heats nothing)")
    watch.add_argument("--seconds", type=float, default=None, help="stop after this long")
    heat = commands.add_parser("heat", parents=[common, heating],
                               help="one heater on, every PT100 printed each second, "
                                    "the one that warmed named (writes nothing)")
    heat.add_argument("heater", help="H0..H5")
    heat.add_argument("--seconds", type=float, default=defaults.max_heat_s,
                      help="longest the heater stays on (default %(default)s)")
    heat.add_argument("--after-s", type=float, default=15.0,
                      help="keep printing this long after it goes off (default %(default)s)")
    auto = commands.add_parser("auto", parents=[common, heating, writing],
                               help="heat every heater in turn and write the pairs")
    auto.add_argument("--check", action="store_true",
                      help="preflight only: report readiness and the RTD terminals, heat nothing")
    auto.add_argument("--max-heat-s", type=float, default=defaults.max_heat_s,
                      help="longest one heater stays on without a terminal warming "
                           "(default %(default)s)")
    auto.add_argument("--verbose", action="store_true",
                      help="print every terminal on every reading while heating")
    assign = commands.add_parser(
        "assign", parents=[common, writing],
        help="write heater/PT100 pairs and motor groups given by hand",
        description="Each motor's specimens, each a PT100 card terminal and, when "
                    "heated, its heater's BCM line: --motor0 ch8:19,ch2:13,ch3:6,ch4:5 "
                    "--motor1 ch5:24,ch7:23,ch1,ch6. Heated specimens take the motor's "
                    "heated samples in the order given (list the MAX31865 click specimen "
                    "first), unheated ones the rest. `show` prints the current config "
                    "in this form.")
    assign.add_argument("--motor0", required=True, metavar="SPECIMENS",
                        help="motor 0's specimens, e.g. ch8:19,ch2:13,ch3:6,ch4:5")
    assign.add_argument("--motor1", required=True, metavar="SPECIMENS",
                        help="motor 1's specimens, e.g. ch5:24,ch7:23,ch1,ch6")
    return root


def main(argv: Optional[list[str]] = None, *,
         send: Callable[[str, str, int], str] = hardware_setup.send_command,
         clock: Callable[[], float] = time.monotonic,
         sleep: Callable[[float], None] = time.sleep,
         ask: Callable[[str], str] = input) -> int:
    """auto: 0 every heater paired and written, 1 not every heater paired (the
    pairs found are still written) or nothing written, 2 could not run, was
    stopped, or the restarted service disagrees. heat: 0 paired, 1 not.
    assign: 0 written, 1 not written, 2 refused or the service disagrees."""
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in (*COMMANDS, "-h", "--help"):
        argv.insert(0, "auto")
    args = parser().parse_args(argv)
    setup = load_setup(args, send, clock, sleep, ask)
    if setup is None:
        return 2
    handlers = {"show": cmd_show, "watch": cmd_watch, "heat": cmd_heat,
                "auto": cmd_auto, "assign": cmd_assign}
    return handlers[args.command](args, setup)


if __name__ == "__main__":
    raise SystemExit(main())
