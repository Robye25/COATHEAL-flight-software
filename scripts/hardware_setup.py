#!/usr/bin/env python3
"""COATHEAL Rev C hardware discovery and commissioning utility."""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "config" / "onboard.local.ini"
EXAMPLE_CONFIG = ROOT / "config" / "onboard.example.ini"
LEGACY_CONFIG = ROOT / "config" / "onboard.ini"
FINAL_PIN_VALUES = {
    # Heater i always reads logical sample i (the ground station pairs them
    # the same way). Which GPIO drives each heater and which RTD card terminal
    # each sample's PT100 landed on are bench wiring (2026-09-14: neither
    # follows the schematic), so heater.output_lines and
    # sensor.sequent_rtd_channels are deliberately NOT pinned here: migration
    # keeps the lines set for the harness and the terminal map
    # scripts/associate_heaters.py measured, and validate_candidate still
    # checks the lines it finds in the INI.
    "heater.temperature_channels": "0,1,2,3,4,5",
    "hal.status_led_enabled": "false",
    "hal.mode_led_enabled": "false",
    # v3: TMC5160, SPI-only motion - no STEP/DIR lines exist.
    "motor0.driver": "tmc5160",
    "motor0.gpio_chip": "/dev/gpiochip0",
    "motor0.spi_device": "/dev/spidev0.0",
    "motor0.cs_line": "22",
    "motor0.enable_line": "20",
    "motor0.sense_resistor_ohm": "0.075",
    "motor1.driver": "tmc5160",
    "motor1.gpio_chip": "/dev/gpiochip0",
    "motor1.spi_device": "/dev/spidev0.0",
    "motor1.cs_line": "27",
    "motor1.enable_line": "21",
    "motor1.sense_resistor_ohm": "0.075",
    "sensor.sequent_rtd_stack": "0",
    "sensor.sequent_rtd_poll_ms": "1000",
    "sensor.sequent_rtd_expect_sensor_type": "pt100",
    "sensor.sequent_rtd_resistance_min_ohm": "60.0",
    "sensor.sequent_rtd_resistance_max_ohm": "390.0",
    "sensor.sequent_rtd_crosscheck_tol_c": "2.0",
    # MAX31865 dual-click sample-resistance instrument (schematic v3/v4).
    # Device paths are fixed by hardware (CE1/GP07 = click 0 = SAMPLE1 =
    # /dev/spidev0.1; CE0/GP08 = click 1 = SAMPLE2 = /dev/spidev0.0), not
    # configurable. sample_indices "0,4" is the owner decision of
    # 2026-08-29: resistance is measured on exactly two specimens, the first
    # of each motor group (S0 on motor 0, S4 on motor 1). The ground
    # station mirrors the same pair in app/gui/state.py RESISTANCE_SAMPLES;
    # change both together.
    "sensor.max31865_reference_ohm": "470.0",
    "sensor.max31865_poll_ms": "1000",
    "sensor.max31865_sample_indices": "0,4",
    "sensor.resistance_source": "max31865_click",
    # Owner hard rule (schematic v3 power budget): never more than 3 heaters
    # energised, never more than 15 W thermal. Tracked here so `pin-check`
    # and the wizard pin the owner's values, and validated in
    # validate_candidate() so a hand-edited INI cannot raise the ceiling
    # HeaterScheduler enforces.
    "power.max_active_heaters": "3",
    "power.max_thermal_w": "15.0",
}
OBSOLETE_CONFIG_KEYS = {
    "stepper.microstep",
    "stepper.microsteps",
    "stepper.max_step_hz",
    "stepper.step_line",
    "stepper.dir_line",
    "stepper.enable_line",
    "stepper.invert_direction",
    "stepper.enable_active_low",
    "motor0.sense_resistor",
    "motor1.sense_resistor",
    "motor0.sense_resistance",
    "motor1.sense_resistance",
    # v3: no STEP/DIR lines exist (TMC5160 SPI-only motion); pulse_high_us
    # was only ever meaningful for GPIO-pulse (STEP/DIR) motion.
    "motor0.step_line",
    "motor0.dir_line",
    "motor0.pulse_high_us",
    "motor1.step_line",
    "motor1.dir_line",
    "motor1.pulse_high_us",
    # v3: the TMC2240 range-select current model (current_range_a_peak ->
    # one of four fixed peak-current ranges) is retired; the TMC5160
    # driver's CalculateCurrent() derives GLOBALSCALER/IRUN/IHOLD directly
    # from run_current_a_rms and sense_resistor_ohm with a continuous
    # scaler, no range selection involved.
    "motor0.current_range_a_peak",
    "motor1.current_range_a_peak",
}
# v3 reserved GPIO lines (BCM), mirroring config.cpp's kReservedGpioLines:
# Sequent RTD HAT lines plus the hardware SPI0 chip-selects, which are wired
# to the MAX31865 sample-resistance clicks and are not available for
# heater/motor use.
RESERVED_GPIO_LINES = (
    (14, "reserved: sequent_hat uart_tx"),
    (15, "reserved: sequent_hat uart_rx"),
    (17, "reserved: sequent_hat rs485_dir"),
    (26, "reserved: sequent_hat intn"),
    (7, "reserved: spi0_ce1 (max31865 sample1)"),
    (8, "reserved: spi0_ce0 (max31865 sample2)"),
)
RETIRED_SENSOR_KEYS = frozenset({
    "sensor.sample_temperature_source",
    "sensor.daq132m_enabled", "sensor.daq132m_auto_discover",
    "sensor.daq132m_poll_ms", "sensor.daq132m_device", "sensor.daq132m_baud",
    "sensor.daq132m_parity", "sensor.daq132m_data_bits",
    "sensor.daq132m_stop_bits", "sensor.daq132m_slave_id",
    "sensor.daq132m_function_code", "sensor.daq132m_register_base",
    "sensor.daq132m_register_count", "sensor.daq132m_c_per_count",
    "sensor.daq132m_c_offset", "sensor.daq132m_enabled_channels",
    "sensor.rtd_click_enabled", "sensor.rtd_click_spi_device",
    "sensor.rtd_click_cs_line", "sensor.rtd_click_drdy_line",
    "sensor.rtd_click_wires", "sensor.rtd_click_sample_channel",
    "sensor.rtd_click_reference_ohm", "sensor.rtd_click_filter_hz",
    "sensor.rtd_click_spi_speed_hz",
})


def serial_candidates() -> list[str]:
    preferred: dict[str, str] = {}
    for path in sorted(glob.glob("/dev/serial/by-id/*")):
        preferred[os.path.realpath(path)] = path
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*"):
        for path in sorted(glob.glob(pattern)):
            preferred.setdefault(os.path.realpath(path), path)
    return list(preferred.values())


def discover() -> dict:
    result = {
        "gpio": sorted(glob.glob("/dev/gpiochip*")),
        "spi": sorted(glob.glob("/dev/spidev*")),
        "serial": serial_candidates(),
        "i2c": sorted(glob.glob("/dev/i2c-*")),
        "i2c_addresses": [],
    }
    if shutil.which("i2cdetect") and Path("/dev/i2c-1").exists():
        probe = subprocess.run(
            ["i2cdetect", "-y", "1"],
            text=True, capture_output=True, check=False,
        )
        for token in probe.stdout.replace(":", " ").split():
            if len(token) == 2:
                try:
                    value = int(token, 16)
                except ValueError:
                    continue
                if 0x03 <= value <= 0x77:
                    result["i2c_addresses"].append(f"0x{value:02x}")
    return result


def replace_ini(text: str, updates: dict[str, str]) -> str:
    found: set[str] = set()
    output: list[str] = []
    for line in text.splitlines():
        key = line.split("=", 1)[0].strip() if "=" in line else ""
        if key in updates:
            output.append(f"{key}={updates[key]}")
            found.add(key)
        else:
            output.append(line)
    for key, value in updates.items():
        if key not in found:
            output.append(f"{key}={value}")
    return "\n".join(output) + "\n"


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.chmod(tmp_name, 0o644)
        except OSError:
            pass
        os.replace(tmp_name, path)
        try:
            os.chmod(path, 0o644)
        except OSError:
            pass
    finally:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)


def _ini_values(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def _number_list(value: str) -> list[int]:
    return [int(piece.strip(), 0) for piece in value.split(",") if piece.strip()]


def validate_candidate(text: str) -> list[str]:
    values = _ini_values(text)
    errors: list[str] = []
    try:
        samples = int(values["hardware.sample_count"])
        heaters = int(values["hardware.heater_count"])
        output_lines = _number_list(values["heater.output_lines"])
        temperature_channels = _number_list(values["heater.temperature_channels"])
    except (KeyError, ValueError) as exc:
        return [f"invalid required mapping: {exc}"]
    if len(output_lines) != heaters:
        errors.append("heater.output_lines count must match hardware.heater_count")
    if len(temperature_channels) != heaters:
        errors.append(
            "heater.temperature_channels count must match hardware.heater_count")
    if len(set(temperature_channels)) != len(temperature_channels):
        errors.append("heater temperature mappings must be unique")
    if any(channel < 0 or channel >= samples for channel in temperature_channels):
        errors.append("sensor channel mapping is outside hardware.sample_count")

    stack = values.get("sensor.sequent_rtd_stack")
    try:
        stack_int = int(stack)
    except (TypeError, ValueError):
        errors.append("sensor.sequent_rtd_stack must be an integer 0..7")
    else:
        if not 0 <= stack_int <= 7:
            errors.append("sensor.sequent_rtd_stack must be 0..7")

    raw_channels = values.get("sensor.sequent_rtd_channels", "")
    channels = [c.strip() for c in raw_channels.split(",") if c.strip()]
    # Mirror config.cpp's check exactly: it compares against the *variable*
    # hardware.sample_count, not a literal 8. `samples` is already parsed
    # from that same key above (part of the required-mapping check at the
    # top of this function), so it is guaranteed present here.
    if len(channels) != samples:
        errors.append(
            "sensor.sequent_rtd_channels must list hardware.sample_count "
            "entries")
    elif len(set(channels)) != len(channels):
        errors.append("sensor.sequent_rtd_channels contains duplicates")
    elif any(not c.isdigit() or not 1 <= int(c) <= 8 for c in channels):
        errors.append("sensor.sequent_rtd_channels entries must be 1..8")

    # pt100 only, mirroring config.cpp. The card and the adapter's Probe()
    # both handle pt1000, but ApplyValidation's card-temperature cross-check
    # hardcodes the PT100 Callendar-Van Dusen curve and the 60-390 ohm window
    # is a PT100 window, so a pt1000 config would mark every channel invalid
    # forever with no diagnostic. Reject at load instead.
    if values.get("sensor.sequent_rtd_expect_sensor_type") != "pt100":
        errors.append("sensor.sequent_rtd_expect_sensor_type must be pt100 "
                      "(pt1000 is recognised but not implemented: the CVD "
                      "cross-check and resistance window are PT100-only)")

    # config.cpp:680-687 - the plausibility window must be a real interval.
    try:
        resistance_min = float(values["sensor.sequent_rtd_resistance_min_ohm"])
        resistance_max = float(values["sensor.sequent_rtd_resistance_max_ohm"])
    except (KeyError, ValueError):
        errors.append("sensor.sequent_rtd_resistance_min_ohm and _max_ohm "
                      "must be numbers")
    else:
        if resistance_min >= resistance_max:
            errors.append("sensor.sequent_rtd_resistance_min_ohm must be "
                          "below sensor.sequent_rtd_resistance_max_ohm")

    # config.cpp:610-621 - "max31865_click" is the v3-shipped default;
    # "disabled", "simulated" and the pre-v3 "sequent_rtd" source stay
    # accepted so a fielded INI still loads.
    if values.get("sensor.resistance_source") not in {
            "sequent_rtd", "disabled", "simulated", "max31865_click"}:
        errors.append("sensor.resistance_source must be disabled, simulated, "
                      "sequent_rtd, or max31865_click")

    # config.cpp's Max31865Loop-adjacent validation block - unconditional
    # (independent of resistance_source), mirroring the sequent_rtd_* keys
    # above: the worker always polls both clicks when their bus is
    # available, regardless of which resistance_source is selected.
    try:
        max31865_reference_ohm = float(values["sensor.max31865_reference_ohm"])
    except (KeyError, ValueError):
        errors.append("sensor.max31865_reference_ohm must be a number")
    else:
        if not math.isfinite(max31865_reference_ohm) or max31865_reference_ohm <= 0.0:
            errors.append("sensor.max31865_reference_ohm must be > 0")

    try:
        max31865_poll_ms = int(values["sensor.max31865_poll_ms"])
    except (KeyError, ValueError):
        errors.append("sensor.max31865_poll_ms must be an integer")
    else:
        if max31865_poll_ms <= 0:
            errors.append("sensor.max31865_poll_ms must be > 0")

    raw_max31865_indices = values.get("sensor.max31865_sample_indices", "")
    raw_max31865_pieces = [
        c.strip() for c in raw_max31865_indices.split(",") if c.strip()]
    if len(raw_max31865_pieces) != 2:
        errors.append(
            "sensor.max31865_sample_indices must have exactly two entries")
    elif any(not c.isdigit() for c in raw_max31865_pieces):
        # Mirrors config.cpp's ParseSizeList failure -- a non-numeric entry
        # is a parse error, kept distinct from the range message below.
        errors.append("sensor.max31865_sample_indices must be numeric")
    else:
        # Parse before comparing, matching config.cpp (which compares the
        # parsed std::size_t values, not the raw INI text): a raw-string
        # comparison would miss "0" vs "00" as a duplicate even though both
        # parse to the same index.
        max31865_indices = [int(c) for c in raw_max31865_pieces]
        if len(set(max31865_indices)) != len(max31865_indices):
            errors.append(
                "sensor.max31865_sample_indices entries must be distinct")
        elif any(index >= samples for index in max31865_indices):
            errors.append("sensor.max31865_sample_indices entries must be "
                          "less than hardware.sample_count")

    # Owner hard rule, mirroring config.cpp's power-cap block: <= 3 active
    # heaters and <= 15.0 W thermal. This is a deliberate, narrow reversal of
    # the "validator scope asymmetry by design" note -- that note said this
    # script validates the keys it WRITES, and these two are now keys it
    # writes (FINAL_PIN_VALUES above). The asymmetry stands everywhere else.
    try:
        max_active_heaters = int(values["power.max_active_heaters"])
    except (KeyError, ValueError):
        errors.append("power.max_active_heaters must be an integer")
    else:
        if not 1 <= max_active_heaters <= 3:
            errors.append("power.max_active_heaters must be 1..3 (owner power "
                          "rule: never more than 3 heaters)")

    try:
        max_thermal_w = float(values["power.max_thermal_w"])
    except (KeyError, ValueError):
        errors.append("power.max_thermal_w must be a number")
    else:
        if not math.isfinite(max_thermal_w) or not 0.0 < max_thermal_w <= 15.0:
            errors.append(
                "power.max_thermal_w must be > 0 and <= 15.0 (owner power rule)")

    runtime_chip = values.get("runtime.gpio_chip", "/dev/gpiochip0")
    gpio_claims: dict[tuple[str, int], str] = {}
    # Reserved lines are claimed first, mirroring config.cpp's claim order,
    # so a heater or motor line colliding with one fails with the reserved
    # owner named in the error.
    gpio_keys = [
        (runtime_chip, owner, line) for line, owner in RESERVED_GPIO_LINES
    ] + [
        (runtime_chip, f"heater.output_lines[{index}]", line)
        for index, line in enumerate(output_lines)
    ]
    for motor in (0, 1):
        chip_key = f"motor{motor}.gpio_chip"
        chip = values.get(chip_key, "")
        if not chip:
            errors.append(f"invalid or missing {chip_key}")
        # v3: no STEP/DIR lines exist (TMC5160 SPI-only motion).
        for suffix in ("cs_line", "enable_line"):
            key = f"motor{motor}.{suffix}"
            try:
                gpio_keys.append((chip, key, int(values[key], 0)))
            except (KeyError, ValueError):
                errors.append(f"invalid or missing {key}")
        driver = values.get(f"motor{motor}.driver")
        if driver == "tmc2240":
            errors.append(f"motor{motor}.driver=tmc2240 is retired; use tmc5160")
        elif driver not in ("tmc5160", "simulated"):
            errors.append(f"motor{motor}.driver must be tmc5160 or simulated")
        try:
            # v3: the TMC2240 range-select current model
            # (current_range_a_peak -> one of four fixed peak-current
            # ranges -> GLOBALSCALER) is retired; the TMC5160 driver's
            # CalculateCurrent() derives GLOBALSCALER/IRUN/IHOLD directly
            # from run_current_a_rms and sense_resistor_ohm with a
            # continuous scaler, no range selection involved.
            # run_current_a_rms gets a flat absolute ceiling plus a
            # sense-resistor-derived physical ceiling instead (mirrors
            # config.cpp's per-motor validation block).
            run_current = float(values[f"motor{motor}.run_current_a_rms"])
            spi_speed = int(values[f"motor{motor}.spi_speed_hz"], 0)
            sense_resistor_ohm = float(
                values[f"motor{motor}.sense_resistor_ohm"])
            if not math.isfinite(run_current) or not 0.0 < run_current <= 3.1:
                errors.append(
                    f"motor{motor}.run_current_a_rms must be in (0, 3.1]")
            if not 0 < spi_speed <= 10_000_000:
                errors.append(
                    f"motor{motor}.spi_speed_hz must be in [1, 10000000]")
            if not math.isfinite(sense_resistor_ohm) or not (
                    0.0 < sense_resistor_ohm < 1.0):
                errors.append(
                    f"motor{motor}.sense_resistor_ohm must be in (0, 1)")
            elif math.isfinite(run_current):
                # TMC5160 hardware ceiling: the chip's fixed full-scale
                # sense voltage (Vfs = 0.325 V, see tmc5160_driver.cpp's
                # kVfs) means peak deliverable current is
                # Vfs/sense_resistor_ohm regardless of GLOBALSCALER/IRUN.
                # The onboard binary's CalculateCurrent() already rejects
                # an unreachable request, but that only surfaces as an
                # unhealthy driver once the service is running; checking
                # it here fails migration/validation loudly at the bench
                # instead. sense_resistor_ohm is already known finite and
                # in (0, 1) here, so the division below is safe.
                max_peak = 0.325 / sense_resistor_ohm
                if run_current * math.sqrt(2.0) > max_peak:
                    errors.append(
                        f"motor{motor}.run_current_a_rms exceeds the "
                        "sense resistor's deliverable current ceiling "
                        "(run_current_a_rms*sqrt(2) must be <= "
                        "0.325/sense_resistor_ohm)")
        except (KeyError, ValueError):
            errors.append(f"invalid or missing motor{motor} electrical setting")
    for chip, owner, line in gpio_keys:
        gpio_id = (chip, line)
        previous = gpio_claims.setdefault(gpio_id, owner)
        if previous != owner:
            errors.append(
                f"{chip} line {line} used by {previous} and {owner}")
    return errors


def _backup_path(path: Path) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return path.with_name(f"{path.name}.bak.{stamp}")


def _candidate_from_existing(existing: Path | None) -> str:
    template = EXAMPLE_CONFIG.read_text(encoding="utf-8")
    template_keys = set(_ini_values(template))
    updates: dict[str, str] = {}
    if existing is not None and existing.exists():
        for key, value in _ini_values(existing.read_text(encoding="utf-8")).items():
            # `key in template_keys` is the primary mechanism today: a
            # retired key can only survive into `updates` if it is also
            # present in EXAMPLE_CONFIG. RETIRED_SENSOR_KEYS is a backstop
            # for the day someone re-adds a retired key to the example INI
            # (e.g. during a merge) — without this explicit blocklist that
            # regression would silently resurrect the key in every migrated
            # field config, with no test catching it until then.
            if (key in template_keys and key not in OBSOLETE_CONFIG_KEYS
                    and key not in RETIRED_SENSOR_KEYS):
                updates[key] = value
    updates.update(FINAL_PIN_VALUES)
    return replace_ini(template, updates)


def _check_with_binary(config_text: str) -> int:
    binary = ROOT / "build" / "onboard" / "coatheal_onboard"
    if not binary.exists():
        return 0
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".ini", delete=False,
    ) as candidate:
        candidate.write(config_text)
        candidate_path = Path(candidate.name)
    try:
        check = subprocess.run(
            [str(binary), "--config", str(candidate_path), "--check-config"],
            check=False,
        )
        return check.returncode
    finally:
        candidate_path.unlink(missing_ok=True)


def migrate_config(args: argparse.Namespace) -> int:
    source = args.migrate_from
    if source is not None and not source.exists():
        source = None
    text = _candidate_from_existing(source)
    errors = validate_candidate(text)
    if errors:
        for error in errors:
            print(f"Configuration error: {error}", file=sys.stderr)
        return 2
    check_rc = _check_with_binary(text)
    if check_rc != 0:
        print("Onboard rejected the migrated configuration.", file=sys.stderr)
        return check_rc
    if source is not None:
        backup = _backup_path(source)
        shutil.copy2(source, backup)
        print(f"Backed up {source} -> {backup}")
    if args.config.exists() and not args.yes:
        answer = input(f"Overwrite {args.config}? [y/N]: ").strip().lower()
        if answer not in ("y", "yes"):
            print("No files changed.")
            return 1
    atomic_write(args.config, text)
    print(f"Configuration migrated and written: {args.config}")
    return 0


def _load_config(path: Path) -> tuple[str, dict[str, str]]:
    text = path.read_text(encoding="utf-8")
    return text, _ini_values(text)


# --- Boot-time GPIO states (config.txt `gpio=` directives) -----------------
#
# Schematic v4 fits no external pull resistors to the EKM014 heater inputs
# or the TMC5160 CS/EN lines, and the Pi powers on with pull-UP on BCM 0-8
# and pull-DOWN on BCM 9-27. So until the service claims its lines, HEATER3
# and HEATER4 (BCM 6/5) idle HIGH and both motor chip-selects and enables
# (BCM 22/27, 20/21) idle LOW -- heaters that may be on, drivers selected and
# enabled. The firmware applies `gpio=` lines from config.txt before the
# kernel boots, which is the earliest anything under software control can
# act. These helpers derive that block from the same INI the service runs
# on, so the two can never disagree; deploy_onboard.sh installs it.

BOOT_GPIO_BEGIN = "# >>> COATHEAL boot-time GPIO states (managed by deploy_onboard.sh; hand edits are overwritten) >>>"
BOOT_GPIO_END = "# <<< COATHEAL boot-time GPIO states <<<"


def _ini_bool(values: dict[str, str], key: str, default: bool) -> bool:
    raw = values.get(key)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def boot_gpio_lines(values: dict[str, str]) -> list[str]:
    """`gpio=` directives holding every heater OFF and every TMC5160
    deselected + disabled from firmware boot, derived from the INI values.

    Output/level/pull are set together on one line per state so a line is
    self-contained: `op,dl,pd` = output, driven low, pulled down (the pull
    is what keeps the level after the kernel's pinctrl reverts a released
    line to input)."""
    by_state: dict[str, set[int]] = {}

    def add(pin_text: str, key: str, state: str) -> None:
        pin = int(pin_text.strip(), 0)
        if not 0 <= pin <= 27:
            raise ValueError(f"{key}: BCM {pin} is outside the 40-pin header range 0-27")
        by_state.setdefault(state, set()).add(pin)

    heater_lines = values.get("heater.output_lines", "")
    if not heater_lines.strip():
        raise ValueError("heater.output_lines is missing; cannot derive boot GPIO states")
    heater_off = "op,dl,pd" if _ini_bool(values, "heater.active_high", True) else "op,dh,pu"
    for piece in heater_lines.split(","):
        if piece.strip():
            add(piece, "heater.output_lines", heater_off)

    for motor in ("motor0", "motor1"):
        cs = values.get(f"{motor}.cs_line", "").strip()
        en = values.get(f"{motor}.enable_line", "").strip()
        if cs:
            add(cs, f"{motor}.cs_line", "op,dh,pu")       # CS is active-low: deselected
        if en:
            en_off = ("op,dh,pu" if _ini_bool(values, f"{motor}.enable_active_low", True)
                      else "op,dl,pd")
            add(en, f"{motor}.enable_line", en_off)

    lines: list[str] = []
    for state in ("op,dl,pd", "op,dh,pu", "op,dh,pd", "op,dl,pu"):
        pins = by_state.get(state)
        if pins:
            lines.append(f"gpio={','.join(str(p) for p in sorted(pins))}={state}")
    return lines


def render_boot_gpio_block(lines: list[str]) -> str:
    body = "\n".join([
        BOOT_GPIO_BEGIN,
        "# Schematic v4 has no external pull resistors: hold the EKM014 heater inputs",
        "# OFF and the TMC5160 chip-selects/enables deselected/disabled from firmware",
        "# boot until coatheal-onboard claims them. Derived from config/onboard.local.ini",
        "# by `scripts/hardware_setup.py boot-gpio`; rerun coatheal-deploy after a pin change.",
        *lines,
        BOOT_GPIO_END,
    ])
    return body + "\n"


def upsert_boot_gpio_block(config_txt: str, block: str) -> str:
    """Replace the managed block in a config.txt text (or append one)."""
    out: list[str] = []
    inside = False
    for line in config_txt.splitlines():
        if line.strip() == BOOT_GPIO_BEGIN:
            inside = True
            continue
        if line.strip() == BOOT_GPIO_END:
            inside = False
            continue
        if not inside:
            out.append(line)
    # Trim trailing blank lines so the block always sits after one blank line.
    while out and not out[-1].strip():
        out.pop()
    text = "\n".join(out)
    if text:
        text += "\n\n"
    return text + block


def boot_gpio(args: argparse.Namespace) -> int:
    if not args.config.exists():
        print(f"Config missing: {args.config}", file=sys.stderr)
        return 2
    _, values = _load_config(args.config)
    try:
        block = render_boot_gpio_block(boot_gpio_lines(values))
    except ValueError as error:
        print(f"boot-gpio: {error}", file=sys.stderr)
        return 1
    if args.install is None:
        sys.stdout.write(block)
        return 0
    target: Path = args.install
    if not target.exists():
        print(f"boot-gpio: {target} does not exist", file=sys.stderr)
        return 2
    current = target.read_text(encoding="utf-8")
    updated = upsert_boot_gpio_block(current, block)
    if updated == current:
        print(f"boot-gpio: {target} already carries the current block")
        return 0
    atomic_write(target, updated)
    print(f"boot-gpio: {target} updated -- a reboot is required for the new boot states to apply")
    return 0


def pin_check(args: argparse.Namespace) -> int:
    if not args.config.exists():
        print(f"Config missing: {args.config}", file=sys.stderr)
        return 2
    text, values = _load_config(args.config)
    errors = validate_candidate(text)
    for key, expected in FINAL_PIN_VALUES.items():
        if key.startswith("sensor.") and key not in {
                "sensor.sequent_rtd_stack",
                "sensor.sequent_rtd_poll_ms",
                "sensor.sequent_rtd_expect_sensor_type",
                "sensor.sequent_rtd_resistance_min_ohm",
                "sensor.sequent_rtd_resistance_max_ohm",
                "sensor.sequent_rtd_crosscheck_tol_c",
                "sensor.max31865_reference_ohm",
                "sensor.max31865_poll_ms",
                "sensor.max31865_sample_indices",
                "sensor.resistance_source"}:
            continue
        actual = values.get(key)
        if actual != expected:
            errors.append(f"{key}={actual!r}, expected {expected!r}")
    if os.name != "nt":
        for path_key in ("runtime.gpio_chip", "motor0.gpio_chip", "motor1.gpio_chip"):
            path = Path(values.get(path_key, ""))
            if path and not path.exists():
                errors.append(f"{path_key} does not exist: {path}")
        for path_key in ("motor0.spi_device", "motor1.spi_device"):
            path = Path(values.get(path_key, ""))
            if path and not path.exists():
                errors.append(f"{path_key} does not exist: {path}")
    if errors:
        for error in errors:
            print(f"pin-check: {error}", file=sys.stderr)
        return 1
    print("pin-check: OK")
    return 0


def wizard(args: argparse.Namespace) -> int:
    found = discover()
    print(json.dumps(found, indent=2))
    updates = {
        **FINAL_PIN_VALUES,
        "motor0.run_current_a_rms": "0.8",
        "motor1.run_current_a_rms": "0.8",
    }
    text = replace_ini(EXAMPLE_CONFIG.read_text(encoding="utf-8"), updates)
    errors = validate_candidate(text)
    if errors:
        for error in errors:
            print(f"Configuration error: {error}", file=sys.stderr)
        return 2
    print(f"Candidate configuration: {args.config}")
    if not args.yes:
        answer = input("Write this configuration? [y/N]: ").strip().lower()
        if answer not in ("y", "yes"):
            print("No files changed.")
            return 1
    binary = ROOT / "build" / "onboard" / "coatheal_onboard"
    if binary.exists():
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", suffix=".ini", delete=False,
        ) as candidate:
            candidate.write(text)
            candidate_path = Path(candidate.name)
        try:
            check = subprocess.run(
                [str(binary), "--config", str(candidate_path), "--check-config"],
                check=False,
            )
            if check.returncode != 0:
                print("Onboard rejected the candidate configuration.",
                      file=sys.stderr)
                return check.returncode
        finally:
            candidate_path.unlink(missing_ok=True)
    atomic_write(args.config, text)
    print("Configuration validated and written.")
    return 0


def send_command(command: str, host: str = "127.0.0.1", port: int = 5000) -> str:
    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.sendall((command + "\n").encode())
        # Read the whole reply line: one recv() may return only part of a
        # long CHECK/STATUS body.
        chunks: list[bytes] = []
        while len(b"".join(chunks)) < 65536:
            data = connection.recv(4096)
            if not data:
                break
            chunks.append(data)
            if b"\n" in data:
                break
        return b"".join(chunks).decode(errors="replace").split("\n", 1)[0].strip()


def motor_test(args: argparse.Namespace) -> int:
    if not args.confirm_motion:
        print("--confirm-motion is required", file=sys.stderr)
        return 2
    if args.steps == 0 or abs(args.steps) > 1000 or not 1.0 <= args.speed <= 25.0:
        print("Use 1..1000 microsteps and a speed of 1..25 Hz.",
              file=sys.stderr)
        return 2
    check_response = send_command(f"CHECK MOTOR{args.motor}")
    print(check_response)
    if "overall=OK" not in check_response:
        return 1
    commands = [
        "ARM",
        f"STEPPER_ENABLE {args.motor}",
        f"STEPPER_SET_SPEED {args.motor} {args.speed}",
        f"STEPPER_MOVE {args.motor} {args.steps}",
    ]
    for command in commands:
        response = send_command(command)
        print(response)
        if command == "ARM" and response.startswith("NACK") and "requires STANDBY" in response:
            continue
        if not response.startswith("ACK"):
            return 1
    time.sleep(max(1.0, abs(args.steps) / max(1.0, args.speed * 4.0) + 1.0))
    for command in (
        f"STEPPER_MOVE {args.motor} {-args.steps}",
        f"STEPPER_DISABLE {args.motor}",
    ):
        response = send_command(command)
        print(response)
        if not response.startswith("ACK"):
            return 1
    return 0


# The RTD token's case is not stable across builds. On real hardware
# SensorManager::ActiveCheck formats the reply itself and emits lowercase
# `sequent_rtd=OK`; on a simulated build it short-circuits and echoes the
# requested component name verbatim, producing `SEQUENT_RTD=OK;simulated=1`.
# Match either, and only as a whole token so `sequent_rtd_error=...` and
# `sequent_rtd_burst=...` cannot satisfy it.
_SEQUENT_RTD_OK_RE = re.compile(
    r"(?<![A-Za-z0-9_])sequent_rtd=OK(?![A-Za-z0-9_])", re.IGNORECASE)


def sequent_rtd_reply_ok(response: str) -> bool:
    """True when a `CHECK SEQUENT_RTD` reply reports the card healthy.

    `overall=OK` alone is not enough: it also covers storage, PWM, motors,
    SPI and comms, so a reply can carry `overall=FAIL` for an unrelated
    reason while the card is fine, or - more importantly - the RTD token must
    be checked explicitly for this command to mean anything.
    """
    return "overall=OK" in response and _SEQUENT_RTD_OK_RE.search(
        response) is not None


def rtd_check(args: argparse.Namespace) -> int:
    # SEQUENT_RTD is the current selector name; it is also what
    # COMPONENT_STATE and this CHECK reply put on the wire (see
    # system_controller.cpp / sensor_manager.cpp). RTD_CLICK/DAQ132M still
    # work as request aliases but the reply never contains "rtd_click=OK".
    response = send_command("CHECK SEQUENT_RTD", args.host, args.port)
    print(response)
    return 0 if sequent_rtd_reply_ok(response) else 1


def heater_test(args: argparse.Namespace) -> int:
    if not args.confirm_load:
        print("--confirm-load is required", file=sys.stderr)
        return 2
    if not 0 <= args.heater <= 5 or not 0.0 <= args.duty <= 0.25 or not 0 < args.seconds <= 10:
        print("Use heater 0..5, duty 0..0.25, seconds 0..10.", file=sys.stderr)
        return 2
    for command in (
        f"ARM_DEBUG {args.debug_token}",
        "ARM",
        f"HEATER_TEST {args.heater} {args.duty} {args.seconds}",
    ):
        response = send_command(command, args.host, args.port)
        print(response)
        if command == "ARM" and response.startswith("NACK") and "requires STANDBY" in response:
            continue
        if not response.startswith("ACK"):
            return 1
    time.sleep(args.seconds + 0.5)
    response = send_command("HEATERS_OFF", args.host, args.port)
    print(response)
    return 0 if response.startswith("ACK") else 1


def doctor(args: argparse.Namespace) -> int:
    rc = pin_check(argparse.Namespace(config=args.config))
    print(json.dumps(discover(), indent=2))
    failures = rc != 0
    for command in ("COMPONENTS", "CHECK SEQUENT_RTD", "CHECK PWM",
                    "CHECK MOTOR0", "CHECK MOTOR1"):
        try:
            response = send_command(command, args.host, args.port)
        except OSError as exc:
            print(f"{command}: command connection failed: {exc}",
                  file=sys.stderr)
            failures = True
            continue
        print(response)
        # Same acceptance rule as `rtd-check` for the RTD command, so the two
        # entry points cannot disagree about whether the card is healthy.
        if command == "CHECK SEQUENT_RTD":
            if not sequent_rtd_reply_ok(response):
                failures = True
        elif command.startswith("CHECK") and "overall=OK" not in response:
            failures = True
    return 1 if failures else 0


def plug_and_play(args: argparse.Namespace) -> int:
    migrate_args = argparse.Namespace(
        config=args.config,
        migrate_from=args.migrate_from,
        yes=args.yes,
    )
    rc = migrate_config(migrate_args)
    if rc != 0:
        return rc
    rc = pin_check(argparse.Namespace(config=args.config))
    if rc != 0:
        return rc
    if not args.skip_build:
        build = subprocess.run(
            ["cmake", "--build", "build", "--parallel", str(args.jobs)],
            cwd=ROOT, check=False,
        )
        if build.returncode != 0:
            print("Build failed. Run cmake -S . -B build first if build/ is missing.",
                  file=sys.stderr)
            return build.returncode
    install = subprocess.run(
        ["bash", str(ROOT / "scripts" / "install_onboard_service.sh"),
         str(ROOT), str(args.config.resolve())],
        cwd=ROOT, check=False,
    )
    if install.returncode != 0:
        return install.returncode
    status = subprocess.run(
        ["systemctl", "is-active", "--quiet", "coatheal-onboard.service"],
        check=False,
    )
    if status.returncode != 0:
        subprocess.run(
            ["journalctl", "-u", "coatheal-onboard.service", "-n", "80",
             "--no-pager"],
            check=False,
        )
        return status.returncode
    print("plug-and-play: service active")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    discover_cmd = commands.add_parser("discover")
    discover_cmd.set_defaults(handler=lambda _: print(json.dumps(discover(), indent=2)) or 0)

    wizard_cmd = commands.add_parser("wizard")
    wizard_cmd.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    wizard_cmd.add_argument("--yes", action="store_true")
    wizard_cmd.set_defaults(handler=wizard)

    migrate = commands.add_parser("migrate-config")
    migrate.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    migrate.add_argument("--migrate-from", type=Path, default=LEGACY_CONFIG)
    migrate.add_argument("--yes", action="store_true")
    migrate.set_defaults(handler=migrate_config)

    pins = commands.add_parser("pin-check")
    pins.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    pins.set_defaults(handler=pin_check)

    boot = commands.add_parser(
        "boot-gpio",
        help="print (or --install into config.txt) the gpio= block that holds heaters "
             "off and motor drivers deselected from firmware boot")
    boot.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    boot.add_argument("--install", type=Path, default=None,
                      help="config.txt to update in place (idempotent); omit to print")
    boot.set_defaults(handler=boot_gpio)

    doctor_cmd = commands.add_parser("doctor")
    doctor_cmd.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    doctor_cmd.add_argument("--host", default="127.0.0.1")
    doctor_cmd.add_argument("--port", type=int, default=5000)
    doctor_cmd.set_defaults(handler=doctor)

    rtd = commands.add_parser("rtd-check")
    rtd.add_argument("--host", default="127.0.0.1")
    rtd.add_argument("--port", type=int, default=5000)
    rtd.set_defaults(handler=rtd_check)

    motor = commands.add_parser("motor-test")
    motor.add_argument("--motor", type=int, choices=(0, 1), required=True)
    motor.add_argument("--steps", type=int, default=200)
    motor.add_argument("--speed", type=float, default=25.0)
    motor.add_argument("--confirm-motion", action="store_true")
    motor.set_defaults(handler=motor_test)

    heater = commands.add_parser("heater-test")
    heater.add_argument("--heater", type=int, required=True)
    heater.add_argument("--duty", type=float, default=0.10)
    heater.add_argument("--seconds", type=float, default=2.0)
    heater.add_argument("--host", default="127.0.0.1")
    heater.add_argument("--port", type=int, default=5000)
    heater.add_argument("--debug-token", default="COATHEAL_DEBUG")
    heater.add_argument("--confirm-load", action="store_true")
    heater.set_defaults(handler=heater_test)

    pap = commands.add_parser("plug-and-play")
    pap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    pap.add_argument("--migrate-from", type=Path, default=LEGACY_CONFIG)
    pap.add_argument("--yes", action="store_true")
    pap.add_argument("--skip-build", action="store_true")
    pap.add_argument("--jobs", type=int, default=2)
    pap.set_defaults(handler=plug_and_play)
    return root


def main() -> int:
    args = parser().parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
