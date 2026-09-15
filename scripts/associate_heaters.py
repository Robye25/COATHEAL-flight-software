#!/usr/bin/env python3
"""Pair each heater with the PT100 that reads its heat, and write the pairs
into the onboard config.

The bench harness does not follow the schematic: heater H<i> (BCM
heater.output_lines[i]) does not necessarily warm the PT100 on RTD card
terminal i+1. With coatheal-onboard running in bench mode, this script takes
the heaters one at a time:

  1. it switches the heater on with HEATER_TEST pulses, each of which lapses
     within seconds on its own, so a dead script cannot leave a heater on;
  2. it watches every RTD card terminal and switches the heater off as soon
     as one terminal has clearly warmed while the others have not;
  3. it waits until no PT100 is still warming (a PT100 lags its specimen),
     then takes the next heater.

The pairs go into sensor.sequent_rtd_channels, so that logical sample S<i>
reads the PT100 heater H<i> warms, and heater.temperature_channels stays
0..5: the ground station and the thermal alarms pair H<i> with S<i>.
migrate-config (every coatheal-deploy) keeps both, so the map is written
once and stays.

A heater that warms no terminal, or warms two alike, is reported and left
out. The heaters that did pair are written anyway; the left-out heater's
sample keeps its terminal unless a paired heater took it. Rerun once that
heater is fixed.

Run it on the Pi, heaters and PT100s connected, motors idle:

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
    line: str
    verdict: str                          # paired / no response / ambiguous / not driven
    channel: Optional[int]                # warmest terminal when the heater went off
    rise_c: float                         # its rise over the reading at switch-on
    runner_up: Optional[tuple[int, float]]
    seconds: float                        # heating time


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
    """Heats the heaters one at a time and finds the terminal each one warms."""

    def __init__(self, onboard: Onboard, settings: Settings,
                 heater_lines: list[str], channels: set[int], *,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep,
                 out: Callable[[str], None] = print,
                 log_path: Optional[Path] = None) -> None:
        self.onboard = onboard
        self.s = settings
        self.heater_lines = heater_lines
        self.channels = sorted(channels)
        self.clock = clock
        self.sleep = sleep
        self.out = out
        self.t0 = clock()
        self.readings: list[Reading] = []
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
        """One poll: every terminal's temperature and every heater's applied
        duty. None when this poll failed; three failures in a row raise."""
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
                seen = (f"{terminal.fault}, {terminal.ohms:.1f} ohm"
                        if terminal is not None else "missing from COMPONENTS")
                raise AssociationError(
                    f"the PT100 on ch{channel} stopped reading ({seen}); "
                    "fix that terminal and rerun")

        duties = []
        for index in range(len(self.heater_lines)):
            try:
                duties.append(float(thermal.get(f"h{index}_duty", "nan")))
            except ValueError:
                duties.append(math.nan)
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

    def test(self, heater: int) -> HeaterResult:
        """Heat one heater until one terminal clearly warms, two warm alike,
        or max_heat_s passes; the heater is off again on return."""
        line = self.heater_lines[heater]
        base = self.baseline()
        # repr, not :g -- a rounded-up duty would exceed heater.debug_max_duty.
        command = f"HEATER_TEST {heater} {self.s.duty!r} {self.s.pulse_s!r}"
        refresh_s = max(self.s.poll_s, self.s.pulse_s / 2.5)
        self.say(f"H{heater} (BCM {line}) on at duty {self.s.duty:g}")
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
                ranked = sorted(((c, reading.temps[c] - level) for c, level in base.items()
                                 if c in reading.temps),
                                key=lambda item: item[1], reverse=True)
                best = ranked[0][1] if ranked else 0.0
                second = ranked[1][1] if len(ranked) > 1 else 0.0
                clear = (best >= self.s.rise_c
                         and second <= max(self.s.noise_c, best / self.s.dominance))
                confirmed = confirmed + 1 if clear else 0
                if confirmed >= 2:
                    verdict = "paired"
                elif undriven >= 3:
                    verdict = "not driven"
                elif not clear and best >= 2.0 * self.s.rise_c:
                    # Two terminals warming together do not separate with more heat.
                    verdict = "ambiguous"
                elif now - last_report >= 5.0:
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
            # to it warms by conduction and would be paired instead.
            blockers.append(f"{', '.join(silent) or 'some terminals'} read no PT100: every "
                            f"one of the {sample_count} probes must read before pairing, or "
                            "a neighbouring specimen's probe can pass for a heater's own")
        hot = [t for t in usable if pt100_c(t.ohms) >= abort_c]
        if hot:
            blockers.append("already at or above the abort limit: "
                            + ", ".join(f"ch{t.channel} {pt100_c(t.ohms):.1f} C" for t in hot))
    return status, terminals, blockers


def _write_ini(path: Path, text: str) -> None:
    owner = path.stat()
    hardware_setup.atomic_write(path, text)
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        os.chown(path, owner.st_uid, owner.st_gid)  # keep it editable by coatheal after sudo


def map_changes(values: dict[str, str], new_map: list[int],
                heater_count: int) -> dict[str, str]:
    """The INI keys (and values) that differ from the measured map."""
    wanted = {
        "sensor.sequent_rtd_channels": ",".join(str(c) for c in new_map),
        # Heater i reads sample i; the wiring lives in the map above.
        "heater.temperature_channels": ",".join(str(i) for i in range(heater_count)),
    }
    return {k: v for k, v in wanted.items() if values.get(k) != v}


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
        out("The map was refused (above), nothing written.")
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
        description="Switch on one heater at a time, find the PT100 that warms, and "
                    "write the pairs into the onboard config.")
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
                      help="longest one heater stays on without a terminal warming "
                           "(default %(default)s)")
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
    """0: every heater paired and the map is in the config. 1: not every
    heater paired (the pairs found are still written), or nothing written.
    2: could not run, stopped, or the restarted service disagrees."""
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
    if settings.duty <= 0.0 or settings.rise_c <= 0.0 or settings.max_heat_s <= 0.0:
        print("Need --duty, --rise-c and --max-heat-s above 0.", file=sys.stderr)
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
              f"{settings.duty:g}, each until a PT100 warms by {settings.rise_c:g} C "
              f"(at most {settings.max_heat_s:.0f} s).")
        return 0

    try:
        tick_hz = float(status.get("tick_hz", "1"))
    except ValueError:
        tick_hz = 1.0
    if tick_hz > 0.0:
        settings.duty_grace_s = max(settings.duty_grace_s, 3.0 / tick_hz)
    running_map = [t.channel for t in sorted(terminals, key=lambda t: t.sample)][:sample_count]
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
              f"supply on? Readings: {log_path}")
        return 1
    new_map = compose_channel_map(association, current_map, sample_count)
    print("\nWiring map (S<i> <- RTD card terminal):")
    for sample, channel in enumerate(new_map):
        if sample in association:
            note = f"H{sample}, measured"
        elif sample < len(heater_lines):
            note = f"H{sample}, NOT measured: do not heat H{sample} until a rerun pairs it"
        else:
            note = "unheated"
        print(f"  S{sample} <- ch{channel}  ({note})")
    print("  Heat cannot tell which unheated terminal is which, or which motor group /\n"
          "  MAX31865 click a specimen belongs to: check motorN.samples and\n"
          "  sensor.max31865_sample_indices against the harness.")
    done = 0 if not problems else 1
    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return done
    print()
    changes = map_changes(values, new_map, len(heater_lines))
    if not changes:
        print(f"{config_path} already carries this map.")
    elif not write_config(config_path, changes, args.yes, ask):
        return 1
    if problems:
        left_out = ", ".join(f"H{h}" for h in sorted(problems))
        print(f"{left_out} left out: fix what is reported above, then rerun to pair "
              f"{'it' if len(problems) == 1 else 'them'}.")
    if not runs_this_config:
        print(f"\nThe service loads {service_path}, not {config_path}: copy the map there "
              f"(or repoint COATHEAL_CONFIG), then restart {SERVICE}.")
        return done
    if not changes and running_map == new_map:
        print("The running service already reads this map.")
    elif args.no_restart:
        print(f"\nRestart the service to load the map: sudo systemctl restart {SERVICE}")
        return done
    elif not args.yes and ask("Restart the service now to load the map? [y/N]: ") \
            .strip().lower() not in ("y", "yes"):
        print(f"Not restarted. Later: sudo systemctl restart {SERVICE}")
        return done
    elif not restart_and_verify(onboard, new_map, clock, sleep):
        return 2
    print("Then prove one loop from the ground station: a small SET_TEMP_TARGET on H<i> "
          "must move S<i>, and only S<i>.")
    return done


if __name__ == "__main__":
    raise SystemExit(main())
