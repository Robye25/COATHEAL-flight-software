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
        os.replace(tmp_name, path)
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

    runtime_chip = values.get("runtime.gpio_chip", "/dev/gpiochip0")
    gpio_claims: dict[tuple[str, int], str] = {}
    gpio_keys = [
        (runtime_chip, f"heater.output_lines[{index}]", line)
        for index, line in enumerate(output_lines)
    ]
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


def wizard(args: argparse.Namespace) -> int:
    found = discover()
    print(json.dumps(found, indent=2))
    serial = found["serial"]
    if len(serial) == 1:
        device = serial[0]
    elif args.yes:
        print(f"Expected one RS485 adapter, found: {serial}", file=sys.stderr)
        return 2
    else:
        device = input(
            "Stable RS485 device path (prefer /dev/serial/by-id/...): "
        ).strip()
        if not device:
            print("A serial device path is required.", file=sys.stderr)
            return 2
    channels = (
        "0,1,2,3,4,5,6,7" if args.yes else
        input("Enabled DAQ channels, zero-based [0,1,2,3,4,5,6,7]: ").strip()
        or "0,1,2,3,4,5,6,7"
    )
    updates = {
        "sensor.daq132m_device": device,
        "sensor.daq132m_enabled_channels": channels,
        "motor0.driver": "tmc2240",
        "motor1.driver": "tmc2240",
        "motor0.run_current_a_rms": "0.8",
        "motor1.run_current_a_rms": "0.8",
        "motor0.current_range_a_peak": "0",
        "motor1.current_range_a_peak": "0",
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


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    discover_cmd = commands.add_parser("discover")
    discover_cmd.set_defaults(handler=lambda _: print(json.dumps(discover(), indent=2)) or 0)

    wizard_cmd = commands.add_parser("wizard")
    wizard_cmd.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    wizard_cmd.add_argument("--yes", action="store_true")
    wizard_cmd.set_defaults(handler=wizard)

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
    return root


def main() -> int:
    args = parser().parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
