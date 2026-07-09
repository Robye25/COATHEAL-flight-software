#!/usr/bin/env python3
"""COATHEAL Rev C hardware discovery and commissioning utility."""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import termios
except ImportError:  # Allows configuration helpers to be tested on Windows.
    termios = None

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "config" / "onboard.local.ini"
EXAMPLE_CONFIG = ROOT / "config" / "onboard.example.ini"
LEGACY_CONFIG = ROOT / "config" / "onboard.ini"
FINAL_PIN_VALUES = {
    "heater.output_lines": "17,18,27,5,6,13",
    "heater.temperature_channels": "0,1,2,3,4,5",
    "hal.status_led_enabled": "false",
    "hal.mode_led_enabled": "false",
    "motor0.driver": "tmc2240",
    "motor0.gpio_chip": "/dev/gpiochip0",
    "motor0.spi_device": "/dev/spidev0.0",
    "motor0.cs_line": "22",
    "motor0.enable_line": "12",
    "motor0.step_line": "19",
    "motor0.dir_line": "26",
    "motor1.driver": "tmc2240",
    "motor1.gpio_chip": "/dev/gpiochip0",
    "motor1.spi_device": "/dev/spidev0.0",
    "motor1.cs_line": "23",
    "motor1.enable_line": "21",
    "motor1.step_line": "24",
    "motor1.dir_line": "20",
    "sensor.sample_temperature_source": "rtd_click_max31865",
    "sensor.daq132m_enabled": "false",
    "sensor.rtd_click_enabled": "true",
    "sensor.rtd_click_spi_device": "/dev/spidev0.0",
    "sensor.rtd_click_cs_line": "16",
    "sensor.rtd_click_drdy_line": "25",
    "sensor.rtd_click_wires": "3",
    "sensor.rtd_click_sample_channel": "1",
    "sensor.rtd_click_reference_ohm": "400.0",
    "sensor.rtd_click_filter_hz": "50",
    "sensor.rtd_click_spi_speed_hz": "500000",
    "motor0.current_range_a_peak": "0",
    "motor1.current_range_a_peak": "0",
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
}


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
        enabled_channels = _number_list(
            values["sensor.daq132m_enabled_channels"])
    except (KeyError, ValueError) as exc:
        return [f"invalid required mapping: {exc}"]
    if len(output_lines) != heaters:
        errors.append("heater.output_lines count must match hardware.heater_count")
    if len(temperature_channels) != heaters:
        errors.append(
            "heater.temperature_channels count must match hardware.heater_count")
    if len(set(temperature_channels)) != len(temperature_channels):
        errors.append("heater temperature mappings must be unique")
    if any(channel < 0 or channel >= samples
           for channel in temperature_channels + enabled_channels):
        errors.append("sensor channel mapping is outside hardware.sample_count")
    if len(set(enabled_channels)) != len(enabled_channels):
        errors.append("DAQ enabled channels contain duplicates")
    if values.get("sensor.sample_temperature_source") not in {
            "daq132m_modbus", "rtd_click_max31865"}:
        errors.append("unsupported sensor.sample_temperature_source")
    if values.get("sensor.sample_temperature_source") == "rtd_click_max31865":
        if values.get("sensor.rtd_click_enabled") != "true":
            errors.append("sensor.rtd_click_enabled must be true for RTD Click")
    if values.get("sensor.sample_temperature_source") == "daq132m_modbus":
        if values.get("sensor.daq132m_enabled") != "true":
            errors.append("sensor.daq132m_enabled must be true for DAQ source")
    try:
        rtd_channel = int(values["sensor.rtd_click_sample_channel"], 0)
        rtd_ref = float(values["sensor.rtd_click_reference_ohm"])
        rtd_filter = int(values["sensor.rtd_click_filter_hz"], 0)
        rtd_speed = int(values["sensor.rtd_click_spi_speed_hz"], 0)
        if not 0 <= rtd_channel < samples:
            errors.append("sensor.rtd_click_sample_channel out of range")
        if not math.isfinite(rtd_ref) or rtd_ref <= 0.0:
            errors.append("sensor.rtd_click_reference_ohm must be > 0")
        if rtd_filter not in (50, 60):
            errors.append("sensor.rtd_click_filter_hz must be 50 or 60")
        if not 0 < rtd_speed <= 5_000_000:
            errors.append("sensor.rtd_click_spi_speed_hz must be in [1, 5000000]")
    except (KeyError, ValueError):
        errors.append("invalid or missing RTD Click setting")

    runtime_chip = values.get("runtime.gpio_chip", "/dev/gpiochip0")
    gpio_claims: dict[tuple[str, int], str] = {}
    gpio_keys = [
        (runtime_chip, f"heater.output_lines[{index}]", line)
        for index, line in enumerate(output_lines)
    ]
    if values.get("sensor.rtd_click_enabled") == "true":
        try:
            gpio_keys.append((
                runtime_chip, "sensor.rtd_click_cs_line",
                int(values["sensor.rtd_click_cs_line"], 0)))
            gpio_keys.append((
                runtime_chip, "sensor.rtd_click_drdy_line",
                int(values["sensor.rtd_click_drdy_line"], 0)))
        except (KeyError, ValueError):
            errors.append("invalid or missing RTD Click GPIO line")
    for motor in (0, 1):
        chip_key = f"motor{motor}.gpio_chip"
        chip = values.get(chip_key, "")
        if not chip:
            errors.append(f"invalid or missing {chip_key}")
        for suffix in ("cs_line", "step_line", "dir_line", "enable_line"):
            key = f"motor{motor}.{suffix}"
            try:
                gpio_keys.append((chip, key, int(values[key], 0)))
            except (KeyError, ValueError):
                errors.append(f"invalid or missing {key}")
        if values.get(f"motor{motor}.driver") != "tmc2240":
            errors.append(f"motor{motor}.driver must be tmc2240")
        try:
            run_current = float(values[f"motor{motor}.run_current_a_rms"])
            current_range = float(
                values[f"motor{motor}.current_range_a_peak"])
            spi_speed = int(values[f"motor{motor}.spi_speed_hz"], 0)
            pulse_high = int(values[f"motor{motor}.pulse_high_us"], 0)
            if not math.isfinite(run_current) or not 0.0 < run_current <= 2.1:
                errors.append(
                    f"motor{motor}.run_current_a_rms must be in (0, 2.1]")
            if current_range not in (0.0, 1.0, 2.0, 3.0):
                errors.append(
                    f"motor{motor}.current_range_a_peak must be 0, 1, 2, or 3")
            if current_range > 0.0 and run_current * math.sqrt(2.0) > current_range:
                errors.append(
                    f"motor{motor} current does not fit selected peak range")
            requested_peak = run_current * math.sqrt(2.0)
            selected_range = current_range or (
                1.0 if requested_peak <= 1.0 else
                2.0 if requested_peak <= 2.0 else 3.0)
            global_scaler = math.floor(
                requested_peak * 256.0 / selected_range + 0.5)
            if not 32 <= global_scaler <= 256:
                errors.append(
                    f"motor{motor} current produces invalid GLOBALSCALER")
            if not 0 < spi_speed <= 10_000_000:
                errors.append(
                    f"motor{motor}.spi_speed_hz must be in [1, 10000000]")
            if pulse_high < 1:
                errors.append(
                    f"motor{motor}.pulse_high_us must be at least 1")
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
            if key in template_keys and key not in OBSOLETE_CONFIG_KEYS:
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


def pin_check(args: argparse.Namespace) -> int:
    if not args.config.exists():
        print(f"Config missing: {args.config}", file=sys.stderr)
        return 2
    text, values = _load_config(args.config)
    errors = validate_candidate(text)
    for key, expected in FINAL_PIN_VALUES.items():
        if key.startswith("sensor.") and key not in {
                "sensor.rtd_click_cs_line",
                "sensor.rtd_click_drdy_line",
                "sensor.rtd_click_spi_device",
                "sensor.sample_temperature_source",
                "sensor.daq132m_enabled",
                "sensor.rtd_click_enabled",
                "sensor.rtd_click_sample_channel",
                "sensor.rtd_click_reference_ohm",
                "sensor.rtd_click_filter_hz",
                "sensor.rtd_click_spi_speed_hz"}:
            continue
        actual = values.get(key)
        if actual != expected:
            errors.append(f"{key}={actual!r}, expected {expected!r}")
    if os.name != "nt":
        for path_key in ("runtime.gpio_chip", "motor0.gpio_chip", "motor1.gpio_chip"):
            path = Path(values.get(path_key, ""))
            if path and not path.exists():
                errors.append(f"{path_key} does not exist: {path}")
        for path_key in ("motor0.spi_device", "motor1.spi_device",
                         "sensor.rtd_click_spi_device"):
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
    channel = (
        "0" if args.yes else
        input("RTD Click sample channel, zero-based [1]: ").strip() or "1"
    )
    updates = {
        **FINAL_PIN_VALUES,
        "sensor.rtd_click_sample_channel": channel,
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


def modbus_crc(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def configure_serial(fd: int, baud: int, parity: str) -> None:
    if termios is None:
        raise RuntimeError("serial configuration requires Linux")
    baud_constants = {
        1200: termios.B1200, 2400: termios.B2400, 4800: termios.B4800,
        9600: termios.B9600, 19200: termios.B19200,
        38400: termios.B38400, 57600: termios.B57600,
        115200: termios.B115200,
    }
    if baud not in baud_constants:
        raise ValueError(f"unsupported baud: {baud}")
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    if parity == "E":
        attrs[2] |= termios.PARENB
    elif parity == "O":
        attrs[2] |= termios.PARENB | termios.PARODD
    attrs[4] = baud_constants[baud]
    attrs[5] = baud_constants[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def read_registers(
    device: str, baud: int, parity: str, slave: int,
    function: int, base: int, count: int, timeout: float,
) -> list[int] | None:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_SYNC)
    try:
        configure_serial(fd, baud, parity)
        body = struct.pack(">BBHH", slave, function, base, count)
        request = body + struct.pack("<H", modbus_crc(body))
        os.write(fd, request)
        termios.tcdrain(fd)
        expected = 5 + count * 2
        response = bytearray()
        deadline = time.monotonic() + timeout
        while len(response) < expected and time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], deadline - time.monotonic())
            if not ready:
                break
            response.extend(os.read(fd, expected - len(response)))
            if len(response) >= 5 and response[1] & 0x80:
                expected = 5
        if len(response) != expected:
            return None
        if modbus_crc(response[:-2]) != int.from_bytes(response[-2:], "little"):
            return None
        if response[0] != slave or response[1] != function:
            return None
        if response[2] != count * 2:
            return None
        return [
            int.from_bytes(response[3 + 2 * i:5 + 2 * i], "big", signed=True)
            for i in range(count)
        ]
    finally:
        os.close(fd)


def daq_scan(args: argparse.Namespace) -> int:
    if (args.slave_start < 1 or args.slave_end > 247 or
            args.slave_end < args.slave_start or
            args.slave_end - args.slave_start > 31 or
            args.count < 1 or args.count > 8 or args.timeout <= 0 or
            any(base < 0 or base > 65535 for base in args.base) or
            len(args.base) > 16):
        print("Scan bounds are invalid or too broad.", file=sys.stderr)
        return 2
    candidates = serial_candidates()
    device = args.device
    if device == "auto":
        if len(candidates) != 1:
            print(f"Expected one serial adapter, found: {candidates}", file=sys.stderr)
            return 2
        device = candidates[0]
    hits = 0
    for slave in range(args.slave_start, args.slave_end + 1):
        for function in args.function:
            for base in args.base:
                values = read_registers(
                    device, args.baud, args.parity, slave, function,
                    base, args.count, args.timeout,
                )
                if values is None:
                    continue
                hits += 1
                scaled = [value * args.scale + args.offset for value in values]
                print(json.dumps({
                    "device": device, "baud": args.baud, "parity": args.parity,
                    "slave": slave, "function": function, "base": base,
                    "raw": values, "scaled_c": scaled,
                }))
    if hits == 0:
        print("No valid Modbus frame found.", file=sys.stderr)
        return 1
    return 0


def send_command(command: str, host: str = "127.0.0.1", port: int = 5000) -> str:
    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.sendall((command + "\n").encode())
        return connection.recv(4096).decode(errors="replace").strip()


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


def rtd_check(args: argparse.Namespace) -> int:
    response = send_command("CHECK RTD_CLICK", args.host, args.port)
    print(response)
    return 0 if "overall=OK" in response and "rtd_click=OK" in response else 1


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
    for command in ("COMPONENTS", "CHECK RTD_CLICK", "CHECK PWM",
                    "CHECK MOTOR0", "CHECK MOTOR1"):
        try:
            response = send_command(command, args.host, args.port)
        except OSError as exc:
            print(f"{command}: command connection failed: {exc}",
                  file=sys.stderr)
            failures = True
            continue
        print(response)
        if command.startswith("CHECK") and "overall=OK" not in response:
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

    doctor_cmd = commands.add_parser("doctor")
    doctor_cmd.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    doctor_cmd.add_argument("--host", default="127.0.0.1")
    doctor_cmd.add_argument("--port", type=int, default=5000)
    doctor_cmd.set_defaults(handler=doctor)

    rtd = commands.add_parser("rtd-check")
    rtd.add_argument("--host", default="127.0.0.1")
    rtd.add_argument("--port", type=int, default=5000)
    rtd.set_defaults(handler=rtd_check)

    scan = commands.add_parser("daq-scan")
    scan.add_argument("--device", default="auto")
    scan.add_argument("--baud", type=int, default=9600)
    scan.add_argument("--parity", choices=("N", "E", "O"), default="N")
    scan.add_argument("--slave-start", type=int, default=1)
    scan.add_argument("--slave-end", type=int, default=10)
    scan.add_argument("--function", type=int, nargs="+", choices=(3, 4), default=[3, 4])
    scan.add_argument("--base", type=int, nargs="+", default=[0, 1])
    scan.add_argument("--count", type=int, default=8)
    scan.add_argument("--timeout", type=float, default=0.25)
    scan.add_argument("--scale", type=float, default=0.1)
    scan.add_argument("--offset", type=float, default=0.0)
    scan.set_defaults(handler=daq_scan)

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
