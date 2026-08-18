#!/usr/bin/env python3
"""Read-only SPI probe for COATHEAL Rev C bench hardware.

The onboard service owns several GPIO lines during normal operation. Stop
coatheal-onboard.service before using this script so the diagnostic can claim
software chip-select lines directly.
"""

from __future__ import annotations

import argparse
import fcntl
import sys
import time
from dataclasses import dataclass
from typing import Iterable, List, Sequence

# spidev/gpiod are needed only by the SPI half of this script. The Sequent RTD
# check below is pure I2C through /dev/i2c-1 and needs neither, so a missing
# SPI stack must not abort the whole probe at import time - that is exactly the
# bench situation this script has to survive (a host with I2C brought up and
# the SPI packages not installed yet). Record why the import failed and let
# main() decide, per section, whether that is fatal.
#
# `from __future__ import annotations` above keeps the `spidev.SpiDev`
# annotations on transfer()/read_tmc2240() from being evaluated, so leaving
# these names as None is safe until something actually calls into them.
try:
    import spidev  # type: ignore
except ImportError as exc:  # pragma: no cover - platform diagnostic
    spidev = None  # type: ignore[assignment]
    SPI_IMPORT_ERROR = f"python3-spidev is not installed ({exc})"
else:
    SPI_IMPORT_ERROR = ""

try:
    import gpiod  # type: ignore
    from gpiod.line import Direction, Value  # type: ignore
except ImportError as exc:  # pragma: no cover - platform diagnostic
    gpiod = None  # type: ignore[assignment]
    Direction = Value = None  # type: ignore[assignment]
    GPIOD_IMPORT_ERROR = f"python3-libgpiod/gpiod is not installed ({exc})"
else:
    GPIOD_IMPORT_ERROR = ""

I2C_SLAVE = 0x0703
RTD_STACK_MIN = 0
RTD_STACK_MAX = 7
RTD_ADDRESS_BASE = 0x40


@dataclass(frozen=True)
class Device:
    name: str
    cs_line: int
    kind: str


DEVICES = (
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


def spi_support_error() -> str:
    """Empty string when the SPI half of this script can run, else why not."""
    return SPI_IMPORT_ERROR or GPIOD_IMPORT_ERROR


def probe_sequent_rtd(stack: int = 0) -> int:
    """Read firmware revision from the Sequent RTD card, as doBoardInit does."""
    # Validate before deriving the address: the card only decodes stack 0..7
    # (SequentRtdAdapter::kStackMin/kStackMax), so anything else would either
    # compute an address the card cannot answer at or, for a negative stack,
    # walk *below* 0x40 onto some other device's address.
    if not RTD_STACK_MIN <= stack <= RTD_STACK_MAX:
        raise SystemExit(
            f"--rtd-stack must be {RTD_STACK_MIN}..{RTD_STACK_MAX} "
            f"(I2C 0x{RTD_ADDRESS_BASE:02x}.."
            f"0x{RTD_ADDRESS_BASE + RTD_STACK_MAX:02x}), got {stack}")
    address = RTD_ADDRESS_BASE + stack
    try:
        with open("/dev/i2c-1", "r+b", buffering=0) as bus:
            fcntl.ioctl(bus, I2C_SLAVE, address)
            bus.write(bytes([57]))          # REVISION_MAJOR_MEM_ADD
            data = bus.read(2)
    except OSError as exc:
        print(f"    sequent rtd stack={stack} addr=0x{address:02x}: {exc}")
        return 1
    print(f"    sequent rtd stack={stack} addr=0x{address:02x} "
          f"fw={data[0]}.{data[1]}")
    return 0


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
    parser.add_argument("--rtd-stack", type=int, default=0,
                        help="Sequent RTD HAT stack address offset, 0..7 "
                             "(I2C address 0x40 + stack).")
    parser.add_argument("--skip-rtd", action="store_true",
                        help="Skip the Sequent RTD I2C presence check.")
    args = parser.parse_args()

    speeds = parse_speeds(args.speeds)
    devices = selected_devices(args.device)

    print("This is read-only: no register writes, no motor movement, no heater commands.")

    rtd_rc = 0
    if not args.skip_rtd:
        print("\n=== Sequent RTD HAT (I2C, /dev/i2c-1) ===")
        rtd_rc = probe_sequent_rtd(args.rtd_stack)

    spi_error = spi_support_error()
    if spi_error:
        # Naming --device is an explicit request for the SPI half, so not
        # being able to run it is a failure rather than a skip.
        if args.device:
            raise SystemExit(f"--device requires the SPI stack: {spi_error}")
        print(f"\n=== SPI (TMC2240) SKIPPED: {spi_error} ===")
        print("Install python3-spidev and python3-libgpiod to probe the "
              "motor drivers; the I2C section above ran regardless.")
        return rtd_rc

    spi = spidev.SpiDev()
    spi.open(args.spi_bus, args.spi_device)
    spi.mode = 0b11
    spi.bits_per_word = 8
    if hasattr(spi, "no_cs"):
        spi.no_cs = True

    print(
        f"\nspi_probe bus={args.spi_bus}.{args.spi_device} mode=3 "
        f"no_cs={getattr(spi, 'no_cs', 'unknown')} gpio_chip={args.gpio_chip}"
    )

    try:
        for speed in speeds:
            spi.max_speed_hz = speed
            print(f"\n=== speed {speed} Hz ===")
            for device in devices:
                print(f"  [{device.name}] cs=BCM{device.cs_line}")
                cs = CsLine(args.gpio_chip, device.cs_line)
                try:
                    if device.kind == "tmc2240":
                        read_tmc2240_set(spi, cs)
                    else:
                        raise AssertionError(device.kind)
                finally:
                    cs.close()
    finally:
        spi.close()

    return rtd_rc


if __name__ == "__main__":
    sys.exit(main())
