#!/usr/bin/env python3
"""Read-only SPI probe for COATHEAL Rev C bench hardware.

The onboard service owns several GPIO lines during normal operation. Stop
coatheal-onboard.service before using this script so the diagnostic can claim
software chip-select lines directly.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Iterable, List, Sequence

try:
    import spidev  # type: ignore
except ImportError as exc:  # pragma: no cover - platform diagnostic
    raise SystemExit("python3-spidev is required to run this probe") from exc

try:
    import gpiod  # type: ignore
    from gpiod.line import Direction, Value  # type: ignore
except ImportError as exc:  # pragma: no cover - platform diagnostic
    raise SystemExit("python3-libgpiod/gpiod is required to run this probe") from exc


@dataclass(frozen=True)
class Device:
    name: str
    cs_line: int
    kind: str


DEVICES = (
    Device("RTD_CLICK_MAX31865", 16, "max31865"),
    Device("MOTOR0_TMC2240", 22, "tmc2240"),
    Device("MOTOR1_TMC2240", 23, "tmc2240"),
)


def hex_bytes(values: Sequence[int]) -> str:
    return " ".join(f"{value & 0xff:02x}" for value in values)


class CsLine:
    def __init__(self, chip: str, offset: int) -> None:
        self.offset = offset
        self.request = gpiod.request_lines(
            chip,
            consumer=f"coatheal-spi-probe-{offset}",
            config={
                offset: gpiod.LineSettings(
                    direction=Direction.OUTPUT,
                    active_low=True,
                    output_value=Value.INACTIVE,
                )
            },
        )

    def select(self) -> None:
        self.request.set_value(self.offset, Value.ACTIVE)
        time.sleep(0.00001)

    def release(self) -> None:
        self.request.set_value(self.offset, Value.INACTIVE)
        time.sleep(0.00001)

    def close(self) -> None:
        self.release()
        self.request.release()


def transfer(spi: spidev.SpiDev, cs: CsLine, tx: Sequence[int]) -> List[int]:
    cs.select()
    try:
        rx = spi.xfer2(list(tx))
    finally:
        cs.release()
    return [int(value) & 0xff for value in rx]


def read_max31865(spi: spidev.SpiDev, cs: CsLine) -> None:
    tx = [0x00] + [0x00] * 8
    rx = transfer(spi, cs, tx)
    regs = rx[1:]
    print(f"    max31865 regs 0x00..0x07 tx={hex_bytes(tx)} rx={hex_bytes(rx)}")
    if len(regs) == 8:
        raw16 = ((regs[1] << 8) | regs[2]) & 0xffff
        code = raw16 >> 1
        fault_bit = raw16 & 0x0001
        print(
            "    max31865 decoded "
            f"config=0x{regs[0]:02x} raw_rtd=0x{raw16:04x} "
            f"code={code} fault_bit={fault_bit} fault=0x{regs[7]:02x}"
        )


def read_tmc2240(spi: spidev.SpiDev, cs: CsLine, reg: int, label: str) -> int:
    tx = [reg & 0x7f, 0x00, 0x00, 0x00, 0x00]
    rx1 = transfer(spi, cs, tx)
    rx2 = transfer(spi, cs, tx)
    value = ((rx2[1] << 24) | (rx2[2] << 16) | (rx2[3] << 8) | rx2[4]) & 0xffffffff
    print(
        f"    tmc2240 {label} reg=0x{reg:02x} "
        f"tx={hex_bytes(tx)} rx1={hex_bytes(rx1)} rx2={hex_bytes(rx2)} "
        f"value=0x{value:08x}"
    )
    return value


def read_tmc2240_set(spi: spidev.SpiDev, cs: CsLine) -> None:
    ioin = read_tmc2240(spi, cs, 0x04, "IOIN")
    version = (ioin >> 24) & 0xff
    print(f"    tmc2240 decoded IOIN.VERSION=0x{version:02x} expected=0x40")
    read_tmc2240(spi, cs, 0x01, "GSTAT")
    read_tmc2240(spi, cs, 0x6f, "DRV_STATUS")


def parse_speeds(value: str) -> List[int]:
    speeds = []
    for part in value.split(','):
        part = part.strip()
        if not part:
            continue
        speeds.append(int(part, 0))
    return speeds


def selected_devices(names: Iterable[str]) -> List[Device]:
    wanted = {name.upper() for name in names}
    if not wanted or "ALL" in wanted:
        return list(DEVICES)
    devices = [device for device in DEVICES if device.name.upper() in wanted]
    missing = wanted.difference(device.name.upper() for device in devices)
    if missing:
        raise SystemExit(f"unknown device(s): {', '.join(sorted(missing))}")
    return devices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spi-bus", type=int, default=0)
    parser.add_argument("--spi-device", type=int, default=0)
    parser.add_argument("--gpio-chip", default="/dev/gpiochip0")
    parser.add_argument("--speeds", default="100000,500000,1000000")
    parser.add_argument("--device", action="append", default=[],
                        help="Device name to probe, or ALL. May be repeated.")
    args = parser.parse_args()

    speeds = parse_speeds(args.speeds)
    devices = selected_devices(args.device)

    spi = spidev.SpiDev()
    spi.open(args.spi_bus, args.spi_device)
    spi.mode = 0b11
    spi.bits_per_word = 8
    if hasattr(spi, "no_cs"):
        spi.no_cs = True

    print(
        f"spi_probe bus={args.spi_bus}.{args.spi_device} mode=3 "
        f"no_cs={getattr(spi, 'no_cs', 'unknown')} gpio_chip={args.gpio_chip}"
    )
    print("This is read-only: no register writes, no motor movement, no heater commands.")

    try:
        for speed in speeds:
            spi.max_speed_hz = speed
            print(f"\n=== speed {speed} Hz ===")
            for device in devices:
                print(f"  [{device.name}] cs=BCM{device.cs_line}")
                cs = CsLine(args.gpio_chip, device.cs_line)
                try:
                    if device.kind == "max31865":
                        read_max31865(spi, cs)
                    elif device.kind == "tmc2240":
                        read_tmc2240_set(spi, cs)
                    else:
                        raise AssertionError(device.kind)
                finally:
                    cs.close()
    finally:
        spi.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
