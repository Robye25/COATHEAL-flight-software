from __future__ import annotations

import math
from datetime import datetime, timezone
from typing import List, Optional

from .protocol import PullEvent, StepperSnapshot, TelemetryPacket


class DemoTelemetryScenario:
    """Deterministic Rev-C stand-demo telemetry for a laptop-only ground station."""

    PERIOD_S = 165.0
    PULL_START_TIMES_S = (42.0, 49.0, 56.0, 67.0, 74.0, 81.0, 88.0, 99.0)
    PULL_DURATION_S = 5.0
    PULL_STEPS = 800

    def __init__(
        self,
        *,
        session_id: str = "demo-stand",
        tick_hz: float = 2.0,
        speed: float = 2.0,
    ) -> None:
        self.session_id = session_id
        self.tick_hz = max(0.1, float(tick_hz))
        self.speed = max(0.1, float(speed))
        self.seq = 0
        self.elapsed_s = 0.0
        self._last_pull_ordinal = 0
        self._pending_pulls: list[PullEvent] = []

    def next_packet(self) -> TelemetryPacket:
        previous_elapsed = self.elapsed_s
        self.elapsed_s += (1.0 / self.tick_hz) * self.speed
        self.seq += 1
        self._queue_pull_events(previous_elapsed, self.elapsed_s)

        cycle_t = self.elapsed_s % self.PERIOD_S
        phase = self._phase_for_cycle_time(cycle_t)
        mode = "STANDBY" if phase == "LANDED" else "RUN"
        pressure = self._pressure_mbar(cycle_t)
        ambient_temp = self._ambient_temp_c(cycle_t)
        samples = self._sample_temps_c(cycle_t)
        active_pull = self._active_pull(cycle_t)
        heater_duty = self._heater_duty(cycle_t, samples, active_pull is not None)
        resistance = self._sample_resistance_ohm(cycle_t)
        steppers = self._steppers(cycle_t)
        status = self._status(active_pull is not None)

        primary_stepper = None
        if steppers:
            first = steppers[0]
            primary_stepper = StepperSnapshot(
                position=int(first["position"]),
                target=int(first["target"]),
                hz=float(first["hz"]),
                microstep=int(first["microstep"]),
                enabled=bool(first["enabled"]),
                moving=bool(first["moving"]),
                holding=bool(first["holding"]),
                hold_s=float(first["hold_s"]),
                pulses=int(first["pulses"]),
                source=str(first["source"]),
            )

        return TelemetryPacket(
            session_id=self.session_id,
            seq=self.seq,
            timestamp=_utc_now(),
            rtc_valid=1,
            ambient_temp_c=ambient_temp,
            ambient_pressure_mbar=pressure,
            uv=self._uv_index(cycle_t),
            sample_temps_c=samples,
            heater_duty=heater_duty,
            sample_resistance_ohm=resistance,
            phase=phase,
            status=status,
            mode=mode,
            steppers=steppers,
            stepper=primary_stepper,
        )

    def drain_pull_events(self) -> list[PullEvent]:
        events = self._pending_pulls
        self._pending_pulls = []
        return events

    def _queue_pull_events(self, previous_elapsed: float, current_elapsed: float) -> None:
        previous_count = self._pull_count_by_elapsed(previous_elapsed)
        current_count = self._pull_count_by_elapsed(current_elapsed)
        if current_count <= previous_count:
            return
        for ordinal in range(previous_count + 1, current_count + 1):
            if ordinal <= self._last_pull_ordinal:
                continue
            self._pending_pulls.append(self._pull_event_for_ordinal(ordinal))
            self._last_pull_ordinal = ordinal

    def _pull_count_by_elapsed(self, elapsed: float) -> int:
        cycle_index = int(elapsed // self.PERIOD_S)
        cycle_t = elapsed % self.PERIOD_S
        return cycle_index * len(self.PULL_START_TIMES_S) + sum(
            1
            for pull_t in self.PULL_START_TIMES_S
            if cycle_t >= pull_t + self.PULL_DURATION_S
        )

    def _pull_event_for_ordinal(self, ordinal: int) -> PullEvent:
        zero_based = ordinal - 1
        pull_index = zero_based % len(self.PULL_START_TIMES_S)
        motor_id = 0 if pull_index < len(self.PULL_START_TIMES_S) // 2 else 1
        samples = [0, 1, 2, 3] if motor_id == 0 else [4, 5, 6, 7]
        direction = 1 if motor_id == 0 else -1
        return PullEvent(
            session_id=self.session_id,
            pull_id=ordinal,
            motor_id=motor_id,
            start_ts=_utc_now(),
            steps_moved=direction * self.PULL_STEPS,
            hold_s=2.0,
            samples=samples,
        )

    def _phase_for_cycle_time(self, cycle_t: float) -> str:
        if cycle_t < 38.0:
            return "ASCENT"
        if cycle_t < 108.0:
            return "PRE_FLOAT"
        if cycle_t < 126.0:
            return "FLOAT"
        if cycle_t < 155.0:
            return "DESCENT"
        return "LANDED"

    def _pressure_mbar(self, cycle_t: float) -> float:
        if cycle_t < 38.0:
            return _lerp(1013.0, 150.0, cycle_t / 38.0)
        if cycle_t < 108.0:
            return _lerp(150.0, 96.0, (cycle_t - 38.0) / 70.0) + 1.8 * math.sin(cycle_t * 0.3)
        if cycle_t < 126.0:
            return 96.0 + 2.2 * math.sin(cycle_t * 0.35)
        if cycle_t < 155.0:
            return _lerp(105.0, 840.0, (cycle_t - 126.0) / 29.0)
        return 842.0 + 8.0 * math.sin(cycle_t * 0.22)

    def _ambient_temp_c(self, cycle_t: float) -> float:
        if cycle_t < 38.0:
            return _lerp(18.0, -50.0, cycle_t / 38.0)
        if cycle_t < 126.0:
            return -52.0 + 2.5 * math.sin(cycle_t * 0.2)
        if cycle_t < 155.0:
            return _lerp(-48.0, -8.0, (cycle_t - 126.0) / 29.0)
        return -4.0 + 1.0 * math.sin(cycle_t * 0.3)

    def _sample_temps_c(self, cycle_t: float) -> list[float]:
        if cycle_t < 38.0:
            base = _lerp(12.0, 5.8, cycle_t / 38.0)
        elif cycle_t < 126.0:
            base = 5.8 + 0.5 * math.sin(cycle_t * 0.22)
        elif cycle_t < 155.0:
            base = _lerp(6.0, 11.0, (cycle_t - 126.0) / 29.0)
        else:
            base = 11.5

        samples: list[float] = []
        for i in range(8):
            offset = (i - 3.5) * 0.12
            ripple = 0.18 * math.sin((self.seq * 0.18) + i * 0.8)
            samples.append(base + offset + ripple)
        return samples

    def _heater_duty(
        self,
        cycle_t: float,
        samples: list[float],
        inhibited: bool,
    ) -> list[float]:
        if inhibited or cycle_t >= 155.0:
            return [0.0 for _ in range(6)]
        target = 5.0
        duties: list[float] = []
        for i, temp in enumerate(samples[:6]):
            feed_forward = 0.06 if cycle_t < 30.0 else 0.22 if cycle_t < 126.0 else 0.08
            demand = feed_forward + max(0.0, target - temp) / 8.0
            demand += 0.025 * math.sin(self.seq * 0.12 + i)
            duties.append(_clamp(demand, 0.0, 1.0))
        return duties

    def _sample_resistance_ohm(self, cycle_t: float) -> list[Optional[float]]:
        total_pulls = self._pull_count_by_elapsed(self.elapsed_s)
        values: list[Optional[float]] = []
        for i in range(8):
            if i >= 6:
                values.append(None)
                continue
            affected = 0
            for ordinal in range(1, total_pulls + 1):
                pull_index = (ordinal - 1) % len(self.PULL_START_TIMES_S)
                motor_id = 0 if pull_index < len(self.PULL_START_TIMES_S) // 2 else 1
                if (motor_id == 0 and i < 4) or (motor_id == 1 and i >= 4):
                    affected += 1
            base = 420.0 - i * 2.4
            ripple = 0.8 * math.sin(cycle_t * 0.24 + i)
            values.append(base * (0.95 ** affected) + ripple)
        return values

    def _uv_index(self, cycle_t: float) -> float:
        if 38.0 <= cycle_t < 126.0:
            return 1.8 + 0.22 * math.sin(cycle_t * 0.12)
        if cycle_t < 38.0:
            return _lerp(0.3, 1.5, cycle_t / 38.0)
        return 0.8

    def _status(self, inhibited: bool) -> str:
        heater_state = "HEATER_INHIBITED" if inhibited else "HEATER_ACTIVE"
        return "|".join(
            [
                "SD_OK",
                "USB_OK",
                "I2C_OK",
                "SPI_OK",
                "LINK_OK",
                "T_AMBIENT_OK",
                "P_AMBIENT_OK",
                "UNIFORMITY_OK",
                "OVERTEMP_OK",
                "ENERGY_OK",
                "RS485_OK",
                heater_state,
                "RESISTANCE_OK",
                "DEMO_SOURCE",
            ]
        )

    def _active_pull(self, cycle_t: float) -> Optional[tuple[int, float]]:
        for idx, pull_t in enumerate(self.PULL_START_TIMES_S):
            dt = cycle_t - pull_t
            if 0.0 <= dt < self.PULL_DURATION_S:
                return idx, dt
        return None

    def _steppers(self, cycle_t: float) -> List[dict]:
        active = self._active_pull(cycle_t)
        completed = self._pull_count_by_elapsed(self.elapsed_s)
        motor_completed = [0, 0]
        for ordinal in range(1, completed + 1):
            pull_index = (ordinal - 1) % len(self.PULL_START_TIMES_S)
            motor_completed[0 if pull_index < len(self.PULL_START_TIMES_S) // 2 else 1] += 1

        snaps: list[dict] = []
        for motor_id in (0, 1):
            direction = 1 if motor_id == 0 else -1
            moving = False
            holding = False
            hold_s = 0.0
            target = 0
            position = 0
            source = "phase:PRE_FLOAT" if cycle_t < 108.0 else "phase:FLOAT"

            if active is not None:
                active_index, dt = active
                active_motor = 0 if active_index < len(self.PULL_START_TIMES_S) // 2 else 1
                if active_motor == motor_id:
                    start_pos = 0
                    target = self.PULL_STEPS * direction
                    if dt < 1.5:
                        moving = True
                        frac = dt / 1.5
                        position = int(_lerp(start_pos, target, frac))
                        source = "demo:fatigue_pull"
                    elif dt < 3.5:
                        holding = True
                        hold_s = dt - 1.5
                        position = target
                        source = "demo:fatigue_hold"
                    else:
                        moving = True
                        frac = (dt - 3.5) / max(0.1, self.PULL_DURATION_S - 3.5)
                        position = int(_lerp(target, start_pos, frac))
                        source = "demo:fatigue_release"

            snaps.append(
                {
                    "motor_id": motor_id,
                    "position": int(position),
                    "target": int(target),
                    "hz": 100.0 if moving else 0.0,
                    "microstep": 4,
                    "enabled": cycle_t < 155.0,
                    "moving": moving,
                    "holding": holding,
                    "hold_s": hold_s,
                    "pulses": motor_completed[motor_id] * self.PULL_STEPS + abs(int(position)),
                    "source": source,
                }
            )
        return snaps


def demo_command_response(command: str) -> tuple[str, str]:
    parts = command.strip().split()
    name = parts[0].upper() if parts else "EMPTY"
    bodies = {
        "PING": "pong (demo)",
        "STATUS": "demo telemetry nominal",
        "FORCE_START": "demo mission running",
        "FORCE_STOP": "demo stop acknowledged",
        "HEATERS_OFF": "demo heaters inhibited",
        "RESET_CTRL": "demo controllers reset",
        "SHUTDOWN_SAFE": "demo shutdown acknowledged",
        "ARM_DEBUG": "demo debug armed",
        "DISARM_DEBUG": "demo debug disarmed",
        "SET_HEATER_DUTY": "demo heater override accepted",
        "SET_ALL_DUTY": "demo heater override accepted",
        "CLEAR_OVERRIDES": "demo overrides cleared",
        "SET_TICK_HZ": "demo tick rate accepted",
        "RADIO_SILENCE": "demo radio silence acknowledged",
        "RADIO_RESUME": "demo radio resumed",
        "ENTER_SAFE": "demo safe mode acknowledged",
        "EXIT_SAFE": "demo run mode acknowledged",
        "STEPPER_STOP": "demo stepper stop acknowledged",
    }
    return name, bodies.get(name, "demo command accepted")


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * _clamp(t, 0.0, 1.0)


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))
