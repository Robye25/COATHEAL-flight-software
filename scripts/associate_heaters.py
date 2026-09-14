#!/usr/bin/env python3
"""Measure which PT100 sits on each heater's specimen and write that wiring
map into the onboard config.

The bench harness does not follow the schematic, so heater H<i> (BCM
heater.output_lines[i]) is not necessarily read by RTD card terminal i+1.
This script finds out by heat. With coatheal-onboard running in bench mode
it warms ONE heater at a time with HEATER_TEST pulses -- each lapses on its
own within seconds, so a dead script cannot leave a heater on -- watches the
raw resistance of every RTD card terminal (COMPONENTS' sequent_rtd_ch), and
pairs the heater with the terminal that warms.

The result is written as sensor.sequent_rtd_channels, ordered so logical
sample i is the specimen heater i warms, and heater.temperature_channels
stays 0..5: the ground station and the thermal alarms pair heater i with
sample i. migrate-config (every coatheal-deploy) keeps this map.

Run it once on the Pi, heaters and PT100s connected, motors idle:

    python3 scripts/associate_heaters.py --check   # preflight only, no heat
    python3 scripts/associate_heaters.py           # measure, write, restart

Heat cannot tell which unheated terminal is S6 and which S7, nor which motor
group or MAX31865 click a specimen belongs to.
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
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hardware_setup  # noqa: E402  (shared INI and command helpers)

ENV_FILE = Path("/etc/coatheal/env")
SERVICE = "coatheal-onboard.service"
CARD_TERMINALS = tuple(range(1, 9))
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
    # A terminal is paired with the heater once its rise reaches rise_c AND
    # the next-warmest terminal stays within max(noise_c, rise / dominance):
    # a neighbour warming by conduction, or the whole room drifting, must not
    # pass for the heated specimen. Rises are means over evidence_s of reads
    # (COMPONENTS reports 0.1 ohm ~ 0.26 C steps, plus read-to-read scatter).
    rise_c: float = 2.0
    dominance: float = 3.0
    noise_c: float = 0.5
    evidence_s: float = 10.0
    abort_c: float = 50.0         # any terminal this hot ends the run
    max_heat_s: float = 300.0
    # HEATER_TEST length. Re-sent every pulse_s / 2.5, so the heater drops
    # within pulse_s of this script dying.
    pulse_s: float = 5.0
    poll_s: float = 1.0
    baseline_s: float = 20.0
    # Between heaters: wait until no terminal has warmed faster than the room
    # by settle_slope_c_per_min over the last settle_window_s (the lagging
    # PT100 has peaked), at most max_settle_s. The window is long enough that
    # one 0.1 ohm step inside it stays under the slope limit.
    settle_window_s: float = 60.0
    settle_slope_c_per_min: float = 0.6
    max_settle_s: float = 300.0
    max_noise_c: float = 1.0      # baseline scatter that makes a terminal suspect
    duty_grace_s: float = 3.0     # a command lands on the next control tick

    @property
    def target_rise_c(self) -> float:
        # Heat a little past rise_c: the verdict's centred average peaks
        # lower than the trailing one that decides when to stop.
        return 1.25 * self.rise_c

    @property
    def stop_rise_c(self) -> float:
        # Heating stops here even when no single terminal leads: two
        # terminals warming together will not separate with more heat.
        return 2.0 * self.rise_c


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
class Sample:
    t: float
    raw: dict[int, float]     # C per usable terminal, as read
    temps: dict[int, float]   # median of the last three reads: no single spikes
    duties: list[float]       # applied duty per heater (GET_THERMAL)


@dataclass
class Baseline:
    level: dict[int, float]
    noise: dict[int, float]   # worst deviation from a straight-line fit


@dataclass
class HeaterResult:
    heater: int
    line: str
    channel: Optional[int]    # the paired terminal; None unless verdict is ok
    verdict: str              # ok / no response / ambiguous / noisy terminal
    candidate: Optional[int]  # warmest terminal, paired or not
    peak_c: float
    runner_up: Optional[tuple[int, float]]
    heat_s: float


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


def _moving_means(points: list[tuple[float, float]], half_s: float) -> list[float]:
    """Mean of the values within +-half_s of each point (points sorted by t)."""
    means = []
    lo = hi = 0
    total = 0.0
    for t, _ in points:
        while hi < len(points) and points[hi][0] <= t + half_s:
            total += points[hi][1]
            hi += 1
        while points[lo][0] < t - half_s:
            total -= points[lo][1]
            lo += 1
        means.append(total / (hi - lo))
    return means


def _line_fit(points: list[tuple[float, float]]) -> tuple[float, float, float]:
    """Least-squares slope (per second), plus the means it pivots on."""
    mean_t = sum(t for t, _ in points) / len(points)
    mean_v = sum(v for _, v in points) / len(points)
    spread = sum((t - mean_t) ** 2 for t, _ in points)
    if spread <= 0.0:
        return 0.0, mean_t, mean_v
    slope = sum((t - mean_t) * (v - mean_v) for t, v in points) / spread
    return slope, mean_t, mean_v


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


class Survey:
    """Heats each heater in turn and pairs it with the terminal that warms."""

    def __init__(self, onboard: Onboard, settings: Settings,
                 heater_lines: list[str], usable: set[int], *,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep,
                 out: Callable[[str], None] = print,
                 log_path: Optional[Path] = None) -> None:
        self.onboard = onboard
        self.s = settings
        self.heater_lines = heater_lines
        self.usable = sorted(usable)
        self.clock = clock
        self.sleep = sleep
        self.out = out
        self.t0 = clock()
        self.samples: list[Sample] = []
        self._recent = {c: deque(maxlen=3) for c in self.usable}
        self._dropouts = dict.fromkeys(self.usable, 0)
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
        self.out(f"[{self.clock() - self.t0:6.0f}s] {message}")

    def run(self) -> list[HeaterResult]:
        self.onboard.ack("HEATERS_OFF")
        self.say("heaters off (any operator targets cleared); waiting for a steady baseline")
        self.settle("baseline")
        base = self.baseline()
        results = []
        for heater, line in enumerate(self.heater_lines):
            first = len(self.samples)
            self.say(f"H{heater} (BCM {line}) on at duty {self.s.duty:g}")
            stopped, heat_s = self.heat(heater, base)
            self.onboard.ack("HEATERS_OFF")
            self.say(f"H{heater} off after {heat_s:.0f} s ({stopped}); letting it cool")
            self.settle(f"H{heater}")
            result = self.evaluate(heater, base, first, heat_s)
            results.append(result)
            self.say(describe(result))
            base = self.baseline()
        return results

    def poll(self, phase: str, heater: Optional[int]) -> Optional[Sample]:
        self.sleep(self.s.poll_s)
        try:
            components = self.onboard.ack("COMPONENTS")
            thermal = self.onboard.ack("GET_THERMAL")
            rtd_error = components.get("sequent_rtd_error", "")
            if rtd_error not in FRESH_RTD_ERRORS:
                raise AssociationError(
                    f"RTD card not reading (sequent_rtd_error={rtd_error or '?'})")
        except (OSError, AssociationError) as error:
            self._failed_polls += 1
            if self._failed_polls >= 3:
                raise AssociationError(f"no reading for 3 polls: {error}") from error
            return None
        self._failed_polls = 0

        by_channel: dict[int, Terminal] = {}
        for terminal in parse_terminals(components.get("sequent_rtd_ch", "")):
            by_channel.setdefault(terminal.channel, terminal)
        raw: dict[int, float] = {}
        temps: dict[int, float] = {}
        for channel in self.usable:
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
                seen = (f"{terminal.fault}, {terminal.ohms:.1f} ohm"
                        if terminal is not None else "missing from COMPONENTS")
                raise AssociationError(
                    f"the PT100 on ch{channel} dropped out ({seen}); "
                    "fix that terminal and rerun")

        duties = []
        for index in range(len(self.heater_lines)):
            try:
                duties.append(float(thermal.get(f"h{index}_duty", "nan")))
            except ValueError:
                duties.append(math.nan)
        sample = Sample(self.clock(), raw, temps, duties)
        self.samples.append(sample)
        if self._log is not None:
            self._log.writerow(
                [f"{sample.t - self.t0:.1f}", phase, "" if heater is None else heater]
                + [f"{by_channel[c].ohms:.1f}" if c in by_channel else ""
                   for c in CARD_TERMINALS]
                + [f"{d:g}" for d in duties])

        hot = {c: v for c, v in temps.items() if v >= self.s.abort_c}
        if hot:
            channel = max(hot, key=hot.get)
            message = (f"ch{channel} reads {hot[channel]:.1f} C, at or above the "
                       f"{self.s.abort_c:g} C abort limit")
            earlier = next((s for s in reversed(self.samples[:-1])
                            if channel in s.temps), None)
            if earlier is not None and sample.t > earlier.t:
                rate = (hot[channel] - earlier.temps[channel]) / (sample.t - earlier.t)
                if rate > 2.0:
                    message += (f" (+{rate:.0f} C/s: a loose PT100 terminal "
                                "looks like this, real heat does not)")
            raise AssociationError(message)
        return sample

    def recent_rises(self, base: Baseline) -> dict[int, float]:
        """Mean rise per terminal over the last evidence_s / 2 of reads.

        No room-drift correction on purpose: by the last heaters most other
        terminals are specimens still cooling from earlier runs, which drags
        any "room" estimate down and inflates every neighbour. Uniform drift
        cannot fake a pairing anyway -- it fails the dominance test."""
        end = self.samples[-1].t
        sums: dict[int, float] = {}
        counts: dict[int, int] = {}
        for sample in reversed(self.samples):
            if sample.t < end - self.s.evidence_s / 2.0:
                break
            for channel, value in sample.raw.items():
                if channel in base.level:
                    sums[channel] = sums.get(channel, 0.0) + value - base.level[channel]
                    counts[channel] = counts.get(channel, 0) + 1
        return {c: sums[c] / counts[c] for c in sums}

    def dominant(self, best: float, second: float) -> bool:
        return (best >= self.s.rise_c
                and second <= max(self.s.noise_c, best / self.s.dominance))

    def baseline(self) -> Baseline:
        end = self.samples[-1].t
        window = [s for s in self.samples if s.t >= end - self.s.baseline_s]
        level: dict[int, float] = {}
        noise: dict[int, float] = {}
        for channel in self.usable:
            points = [(s.t, s.temps[channel]) for s in window if channel in s.temps]
            if len(points) < 3:
                continue
            slope, mean_t, mean_v = _line_fit(points)
            level[channel] = statistics.median(s.raw[channel] for s in window if channel in s.raw)
            noise[channel] = max(abs(v - mean_v - slope * (t - mean_t)) for t, v in points)
        return Baseline(level, noise)

    def quiet(self) -> bool:
        end = self.samples[-1].t
        window = [s for s in self.samples if s.t >= end - self.s.settle_window_s]
        slopes = []
        for channel in self.usable:
            # Raw reads: read-to-read scatter averages out of a slope, while
            # the median filter would turn it into steps.
            points = [(s.t, s.raw[channel]) for s in window if channel in s.raw]
            if len(points) >= 5:
                slopes.append(_line_fit(points)[0])
        limit = self.s.settle_slope_c_per_min / 60.0
        return bool(slopes) and all(slope <= limit for slope in slopes)

    def settle(self, label: str) -> bool:
        start = self.clock()
        min_s = max(self.s.settle_window_s, self.s.baseline_s)
        last_report = start
        driven = 0
        while True:
            sample = self.poll("settle", None)
            now = self.clock()
            if sample is not None:
                on = [i for i, duty in enumerate(sample.duties) if duty > 0.0]
                driven = driven + 1 if on and now - start >= self.s.duty_grace_s else 0
                if driven >= 2:
                    raise AssociationError(
                        f"H{on[0]} is driven (duty {sample.duties[on[0]]:g}) although "
                        "this script switched the heaters off: something else is "
                        "commanding heaters")
                if now - start >= min_s and self.quiet():
                    return True
            if now - start >= self.s.max_settle_s:
                self.say(f"{label}: still warming after {self.s.max_settle_s:.0f} s; "
                         "carrying on")
                return False
            if now - start >= min_s and now - last_report >= 30.0:
                self.say(f"{label}: a terminal is still warming; waiting")
                last_report = now

    def heat(self, heater: int, base: Baseline) -> tuple[str, float]:
        # repr, not :g -- a rounded-up duty would exceed heater.debug_max_duty.
        command = f"HEATER_TEST {heater} {self.s.duty!r} {self.s.pulse_s!r}"
        refresh_s = max(self.s.poll_s, self.s.pulse_s / 2.5)
        start = self.clock()
        self._pulse(command)
        last_pulse = last_report = start
        pulse_failures = undriven = others_on = confirmed = 0
        while True:
            sample = self.poll("heat", heater)
            now = self.clock()
            elapsed = now - start
            if now - last_pulse >= refresh_s:
                try:
                    self._pulse(command)
                except OSError as error:
                    pulse_failures += 1
                    if pulse_failures >= 3:
                        raise AssociationError(f"{command} not reaching the onboard: {error}") from error
                else:
                    last_pulse = now
                    pulse_failures = 0
            if sample is not None and elapsed >= self.s.duty_grace_s:
                # Verify the onboard really drives this heater and only this
                # one, or "no response" would be blamed on the wiring.
                duty = sample.duties[heater]
                undriven = 0 if duty > 0.0 else undriven + 1
                if undriven >= 3:
                    raise AssociationError(
                        f"H{heater} is commanded but the onboard applies duty {duty:g} "
                        "(motor moving? energy budget latched? disarmed by another client?)")
                on = [i for i, d in enumerate(sample.duties) if d > 0.0 and i != heater]
                others_on = others_on + 1 if on else 0
                if others_on >= 2:
                    raise AssociationError(
                        f"H{on[0]} came on while H{heater} was under test: something "
                        "else is commanding heaters")
            if sample is not None:
                ranked = sorted(self.recent_rises(base).items(),
                                key=lambda item: item[1], reverse=True)
                if ranked:
                    channel, best = ranked[0]
                    second = ranked[1][1] if len(ranked) > 1 else 0.0
                    if best >= self.s.stop_rise_c:
                        return f"ch{channel} rose {best:.1f} C", elapsed
                    leads = (best >= self.s.target_rise_c
                             and self.dominant(best, second))
                    confirmed = confirmed + 1 if leads else 0
                    if confirmed >= 2:
                        return f"ch{channel} responded", elapsed
                    if now - last_report >= 15.0:
                        self.say(f"H{heater}: {elapsed:.0f} s, warmest "
                                 + ", ".join(f"ch{c} {v:+.1f} C" for c, v in ranked[:3]))
                        last_report = now
            if elapsed >= self.s.max_heat_s:
                return "time limit", elapsed

    def _pulse(self, command: str) -> None:
        reply = self.onboard.raw(command)
        if not reply.startswith("ACK"):
            raise AssociationError(f"{command} -> {reply or 'no reply'}")

    def evaluate(self, heater: int, base: Baseline, first: int,
                 heat_s: float) -> HeaterResult:
        # The warmest moment over heating AND cool-down (a lagging PT100 keeps
        # rising after the heater is off), on evidence_s means of the reads.
        # Every other terminal is judged at that same moment, not at its own
        # best: a stray excursion elsewhere in the window is not evidence, and
        # a neighbour warmed by conduction peaks later than the specimen.
        window = self.samples[first:]
        series: dict[int, tuple[list[tuple[float, float]], list[float]]] = {}
        for channel, level in base.level.items():
            points = [(s.t, s.raw[channel] - level) for s in window if channel in s.raw]
            if points:
                series[channel] = (points, _moving_means(points, self.s.evidence_s / 2.0))
        line = self.heater_lines[heater]
        if not series:
            return HeaterResult(heater, line, None, "no response", None, 0.0, None, heat_s)
        channel, peak_t, best = None, 0.0, -math.inf
        for candidate, (points, means) in series.items():
            index = max(range(len(means)), key=means.__getitem__)
            if means[index] > best:
                channel, peak_t, best = candidate, points[index][0], means[index]
        at_peak = {}
        for candidate, (points, means) in series.items():
            if candidate != channel:
                index = min(range(len(points)), key=lambda i: abs(points[i][0] - peak_t))
                at_peak[candidate] = means[index]
        runner_up = max(at_peak.items(), key=lambda item: item[1]) if at_peak else None
        if best < self.s.rise_c:
            verdict = "no response"
        elif not self.dominant(best, runner_up[1] if runner_up else 0.0):
            verdict = "ambiguous"
        elif base.noise.get(channel, 0.0) > self.s.max_noise_c:
            verdict = "noisy terminal"
        else:
            verdict = "ok"
        return HeaterResult(heater, line, channel if verdict == "ok" else None,
                            verdict, channel, best, runner_up, heat_s)


def describe(result: HeaterResult) -> str:
    runner = (f", next ch{result.runner_up[0]} {result.runner_up[1]:+.1f} C"
              if result.runner_up else "")
    if result.verdict == "ok":
        return (f"H{result.heater} (BCM {result.line}) -> ch{result.channel}: "
                f"peak {result.peak_c:+.1f} C{runner}")
    best = f"ch{result.candidate} {result.peak_c:+.1f} C" if result.candidate else "nothing"
    return f"H{result.heater} (BCM {result.line}): {result.verdict} (warmest {best}{runner})"


def check_ready(onboard: Onboard, heater_lines: list[str], sample_count: int,
                abort_c: float, out: Callable[[str], None] = print
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
    if running_heaters != len(heater_lines):
        blockers.append(f"the running onboard has {running_heaters} heaters but the config "
                        f"lists {len(heater_lines)}: restart the service on this config first")
    unclaimed = [f"H{i}" for i in range(len(heater_lines))
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
            reading = (f"{pt100_c(terminal.ohms):6.1f} C" if terminal.conducting
                       else "  no probe")
            out(f"  ch{terminal.channel} -> S{terminal.sample}  {terminal.fault:<8} "
                f"{terminal.ohms:8.1f} ohm {reading}")
        usable = [t for t in terminals if t.conducting]
        silent = [f"ch{t.channel}" for t in terminals if not t.conducting]
        if len(usable) < sample_count:
            # With a heater's own probe missing, the PT100 of the specimen next
            # to it warms by conduction and would be paired instead -- and if
            # that neighbour is unheated nothing downstream would catch it.
            blockers.append(f"{', '.join(silent) or 'some terminals'} read no PT100: every "
                            f"one of the {sample_count} probes must read before pairing, or "
                            "a neighbouring specimen's probe can pass for a heater's own")
        hot = [t for t in usable if pt100_c(t.ohms) >= abort_c]
        if hot:
            blockers.append("already at or above the abort limit: "
                            + ", ".join(f"ch{t.channel} {pt100_c(t.ohms):.1f} C" for t in hot))
    return status, terminals, blockers


def complete_association(results: list[HeaterResult], dominance: float,
                         out: Callable[[str], None] = print) -> Optional[dict[int, int]]:
    """heater -> terminal when every heater paired with its own terminal."""
    problems = [r for r in results if r.verdict != "ok"]
    heaters_by_channel: dict[int, list[int]] = {}
    for result in results:
        if result.channel is not None:
            heaters_by_channel.setdefault(result.channel, []).append(result.heater)
    shared = {c: hs for c, hs in heaters_by_channel.items() if len(hs) > 1}
    # A probe that came off its specimen leaves the specimen next door as the
    # warmest terminal. If that neighbour is unheated nothing else catches
    # it, but conducted heat arrives far slower than direct heat -- and the
    # specimens and heaters are all alike, so a laggard is a harness fault.
    typical_s = statistics.median(r.heat_s for r in results) if len(results) >= 3 else 0.0
    slow = [r for r in results if r.verdict == "ok" and typical_s > 0.0
            and r.heat_s > max(3.0 * typical_s, typical_s + 60.0)]
    if not problems and not shared and not slow:
        return {r.heater: r.channel for r in results if r.channel is not None}
    out("\nNo trustworthy map, so nothing was written:")
    for result in slow:
        out(f"  H{result.heater}: needed {result.heat_s:.0f} s to warm ch{result.channel}, "
            f"the others a median {typical_s:.0f} s. A probe off its specimen reads the "
            "neighbouring specimen's heat like this: check that ch"
            f"{result.channel}'s probe is fixed to H{result.heater}'s specimen, then rerun.")
    for result in problems:
        if result.verdict == "no response":
            out(f"  H{result.heater}: no terminal warmed. Heater not connected, its "
                "PT100 not on that specimen, or the specimen heats slowly (try a "
                "longer --max-heat-s).")
        elif result.verdict == "ambiguous":
            share = result.runner_up[1] / result.peak_c
            out(f"  H{result.heater}: ch{result.runner_up[0]} warmed to {share:.0%} of "
                f"ch{result.candidate}'s rise (at most {1.0 / dominance:.0%} allowed). Two "
                "PT100s on one specimen, a heater touching two, a probe off its specimen "
                "(the neighbours then lead), or the room temperature moving: check that "
                "heater's specimen and its probe, then rerun.")
        else:
            out(f"  H{result.heater}: ch{result.candidate} warmed but its baseline "
                "scattered; check that terminal's screws and rerun.")
    for channel, heaters in shared.items():
        out(f"  {' and '.join(f'H{h}' for h in heaters)} all warm ch{channel}: two "
            "heater outputs drive one specimen.")
    return None


def compose_channel_map(association: dict[int, int], current_map: list[int],
                        sample_count: int) -> list[int]:
    """sensor.sequent_rtd_channels with sample i = the specimen heater i warms.
    Terminals no heater warmed fill the unheated samples in the order the
    current map lists them (heat cannot rank them)."""
    heated = [association[h] for h in sorted(association)]
    rest = [c for c in current_map if c not in heated]
    rest += [c for c in CARD_TERMINALS if c not in heated and c not in rest]
    return heated + rest[:sample_count - len(heated)]


def write_config(path: Path, new_map: list[int], heater_count: int, yes: bool,
                 ask: Callable[[str], str] = input,
                 out: Callable[[str], None] = print) -> bool:
    text = path.read_text(encoding="utf-8")
    values = hardware_setup._ini_values(text)
    wanted = {
        "sensor.sequent_rtd_channels": ",".join(str(c) for c in new_map),
        # Heater i reads sample i; the wiring lives in the terminal map above.
        "heater.temperature_channels": ",".join(str(i) for i in range(heater_count)),
    }
    changes = {k: v for k, v in wanted.items() if values.get(k) != v}
    if not changes:
        out(f"{path} already carries this map; nothing to write.")
        return True
    candidate = hardware_setup.replace_ini(text, changes)
    errors = hardware_setup.validate_candidate(candidate)
    if errors:
        for error in errors:
            out(f"Configuration error: {error}")
        return False
    if hardware_setup._check_with_binary(candidate) != 0:
        out("The onboard binary rejected the new config; nothing written.")
        return False
    for key, value in changes.items():
        out(f"  {key}: {values.get(key, '(unset)')} -> {value}")
    if not yes and ask(f"Write this to {path}? [y/N]: ").strip().lower() not in ("y", "yes"):
        out("No files changed.")
        return False
    owner = path.stat()
    backup = hardware_setup._backup_path(path)
    shutil.copy2(path, backup)
    hardware_setup.atomic_write(path, candidate)
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        os.chown(path, owner.st_uid, owner.st_gid)  # keep it editable by coatheal after sudo
    out(f"Backed up {path} -> {backup}")
    out(f"Wrote {path}")
    return True


def restart_and_verify(onboard: Onboard, new_map: list[int],
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
    terminals: list[Terminal] = []
    while not terminals and clock() < deadline:
        sleep(2.0)
        try:
            terminals = parse_terminals(onboard.ack("COMPONENTS").get("sequent_rtd_ch", ""))
        except (OSError, AssociationError):
            terminals = []
    if not terminals:
        out(f"The service did not report RTD terminals within 60 s; see "
            f"journalctl -u {SERVICE}")
        return False
    running = [t.channel for t in sorted(terminals, key=lambda t: t.sample)][:len(new_map)]
    if running != new_map:
        out(f"MISMATCH: the restarted service reads terminals {running}, expected {new_map}")
        return False
    out("Service restarted and reads the new map: "
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


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description="Pair each heater with the PT100 on its specimen by heating "
                    "one heater at a time, and write the map into the onboard config.")
    root.add_argument("--config", type=Path, default=None,
                      help="INI to update (default: the service's COATHEAL_CONFIG, "
                           "else config/onboard.local.ini)")
    root.add_argument("--host", default="127.0.0.1")
    root.add_argument("--port", type=int, default=5000)
    root.add_argument("--check", action="store_true",
                      help="preflight only: report readiness and the RTD terminals, heat nothing")
    root.add_argument("--dry-run", action="store_true",
                      help="measure and report, but write nothing")
    root.add_argument("--yes", action="store_true",
                      help="write the config and restart the service without asking")
    root.add_argument("--no-restart", action="store_true",
                      help="write the config but leave the service on the old map")
    defaults = Settings()
    root.add_argument("--duty", type=float, default=defaults.duty,
                      help="heater duty during a test (capped by heater.debug_max_duty "
                           "and heater.max_duty; default %(default)s)")
    root.add_argument("--rise-c", type=float, default=defaults.rise_c,
                      help="rise that pairs a terminal with the heater (default %(default)s)")
    root.add_argument("--max-heat-s", type=float, default=defaults.max_heat_s,
                      help="longest one heater stays on (default %(default)s)")
    root.add_argument("--abort-c", type=float, default=defaults.abort_c,
                      help="any terminal reaching this ends the run (default %(default)s)")
    root.add_argument("--log", type=Path, default=None,
                      help="CSV of every reading (default logs/heater-association-<time>.csv)")
    return root


def main(argv: Optional[list[str]] = None, *,
         send: Callable[[str, str, int], str] = hardware_setup.send_command,
         clock: Callable[[], float] = time.monotonic,
         sleep: Callable[[float], None] = time.sleep,
         ask: Callable[[str], str] = input) -> int:
    args = parser().parse_args(argv)
    service_path = service_config()
    config_path: Path = args.config or service_path or hardware_setup.DEFAULT_CONFIG
    try:
        values = hardware_setup._ini_values(config_path.read_text(encoding="utf-8"))
        heater_lines = [p.strip() for p in values["heater.output_lines"].split(",") if p.strip()]
        sample_count = int(values.get("hardware.sample_count", "8"))
        current_map = hardware_setup._number_list(
            values.get("sensor.sequent_rtd_channels", "1,2,3,4,5,6,7,8"))
        duty_cap = min(float(values.get("heater.debug_max_duty", "0.25")),
                       float(values.get("heater.max_duty", "1.0")))
        pulse_cap = float(values.get("heater.debug_max_seconds", "10"))
    except (OSError, KeyError, ValueError) as error:
        print(f"Cannot use {config_path}: {error}", file=sys.stderr)
        return 2
    token = values.get("runtime.debug_arm_code", "COATHEAL_DEBUG")
    settings = Settings(duty=min(args.duty, duty_cap), rise_c=args.rise_c,
                        max_heat_s=args.max_heat_s, abort_c=args.abort_c,
                        pulse_s=min(Settings.pulse_s, pulse_cap))
    if settings.duty <= 0.0 or settings.rise_c <= 0.0:
        print("Need --duty > 0 and --rise-c > 0.", file=sys.stderr)
        return 2
    runs_this_config = (service_path is None
                        or service_path.resolve() == config_path.resolve())
    print(f"Config: {config_path}"
          + ("" if runs_this_config else f"  (the service runs {service_path})"))

    onboard = Onboard(args.host, args.port, send)
    try:
        status, terminals, blockers = check_ready(onboard, heater_lines, sample_count,
                                                  settings.abort_c)
    except AssociationError as error:
        blockers = [str(error)]
        status, terminals = {}, []
    if blockers:
        print("\nNOT READY:")
        for blocker in blockers:
            print(f"  - {blocker}")
        return 2
    if args.check:
        print(f"\nReady: {len(heater_lines)} heaters, one at a time at duty "
              f"{settings.duty:g}, up to {settings.max_heat_s:.0f} s each.")
        return 0

    try:
        tick_hz = float(status.get("tick_hz", "1"))
    except ValueError:
        tick_hz = 1.0
    if tick_hz > 0.0:
        settings.duty_grace_s = max(settings.duty_grace_s, 3.0 / tick_hz)
    log_path = args.log or (hardware_setup.ROOT / "logs"
                            / f"heater-association-{time.strftime('%Y%m%d-%H%M%S')}.csv")
    survey = Survey(onboard, settings, heater_lines,
                    {t.channel for t in terminals if t.conducting},
                    clock=clock, sleep=sleep, log_path=log_path)
    print(f"\nHeating {len(heater_lines)} heaters one at a time. Ctrl+C stops safely. "
          f"Readings: {log_path}")

    def stop(signum, frame):
        raise KeyboardInterrupt

    handlers = {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGHUP)}
    armed_debug = armed_run = False
    failure = None
    results: list[HeaterResult] = []
    try:
        if status.get("debug_armed") != "1":
            onboard.ack(f"ARM_DEBUG {token}")
            armed_debug = True
        if status.get("mode") == "STANDBY":
            onboard.ack("ARM")
            armed_run = True
        results = survey.run()
    except AssociationError as error:
        failure = str(error)
    except OSError as error:
        failure = f"lost the onboard ({error})"
    except KeyboardInterrupt:
        failure = "interrupted"
    finally:
        heaters_off = restore(onboard, armed_debug, armed_run, sleep)
        survey.close()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    if not heaters_off:
        print(f"\nWARNING: HEATERS_OFF was not confirmed. The last HEATER_TEST pulse "
              f"lapses by itself within {settings.pulse_s:g} s; check the heaters.")
    if failure is not None:
        print(f"\nSTOPPED: {failure}\nNothing was written.")
        return 2

    print("\nResults:")
    for result in results:
        print(f"  {describe(result)}")
    association = complete_association(results, settings.dominance)
    if association is None:
        return 1
    new_map = compose_channel_map(association, current_map, sample_count)
    print("\nWiring map (sample i = the specimen heater i warms):")
    for sample, channel in enumerate(new_map):
        owner = (f"H{sample}, BCM {heater_lines[sample]}" if sample < len(heater_lines)
                 else "unheated")
        print(f"  S{sample} <- ch{channel}  ({owner})")
    print("  Heat cannot tell which unheated terminal is which, or which motor group /\n"
          "  MAX31865 click a specimen belongs to: check motorN.samples and\n"
          "  sensor.max31865_sample_indices against the harness.")
    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return 0
    if not write_config(config_path, new_map, len(heater_lines), args.yes, ask):
        return 1
    print("Then prove one loop from the ground station: a small SET_TEMP_TARGET on H<i> "
          "must move S<i>, and only S<i>.")
    if not runs_this_config:
        print(f"\nThe service loads {service_path}, not {config_path}: copy the map there "
              f"(or repoint COATHEAL_CONFIG), then restart {SERVICE}.")
        return 0
    if args.no_restart:
        print(f"\nRestart the service to load the map: sudo systemctl restart {SERVICE}")
        return 0
    if not args.yes and ask("Restart the service now to load the map? [y/N]: ").strip().lower() \
            not in ("y", "yes"):
        print(f"Not restarted. Later: sudo systemctl restart {SERVICE}")
        return 0
    return 0 if restart_and_verify(onboard, new_map, clock, sleep) else 2


if __name__ == "__main__":
    raise SystemExit(main())
