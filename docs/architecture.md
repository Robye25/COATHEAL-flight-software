# System Architecture (Rev C)

COATHEAL uses a TCP/IP client-server architecture. The Raspberry Pi runs the
C++ onboard process. The laptop runs the Python/PyQt ground station.

Rev C is manual-first. Operators command phase, heaters, and motor pulls while
the ground link is healthy. The onboard software keeps safety interlocks,
telemetry durability, logging, watchdog recovery, and link-loss fallback.

```text
Raspberry Pi 4
  coatheal_onboard
    SystemController
      CommandServer :5000
      TelemetryClient :4000 + UDP discovery :4100
      TelemetryQueue + StorageManager
      SensorManager
        independent bounded polling workers + thread-safe cache
        DAQ132M over USB-RS485 Modbus: PT100 samples 0..7
        DPS310 over I2C: pressure + ambient temperature
        ADS1115 over I2C: GUVA-S12SD UV analog input
        optional RTD Click over SPI for bench PT100 validation
      ThermalController
        6 manual target PIDs; fallback floor for untargeted channels
      HeaterScheduler
        4 active / 20 W / energy budget / MotionLock gate
      PwmController
        H0..H5 MOSFET input mapping
      StepperController
        M0 + M1 TMC2240 motor channels

Ground station laptop
  gui_app.py
    TelemetryReceiver TCP server :4000
    CommandSender one-shot TCP client :5000
    Discovery beacons UDP :4100
    Live plots, command buttons, CSV logs, ACK cursor
```

## Runtime Data Flow

```text
Sensors
  -> SensorSnapshot
  -> StateManager only if link-loss fallback is active
  -> ThermalController
  -> HeaterScheduler
  -> PwmController
  -> TelemetryRecord
  -> StorageManager + TelemetryQueue
  -> TelemetryClient
  -> Ground station ACK
```

Motor pulls use a separate command path:

```text
Ground command
  -> CommandServer
  -> StepperController
  -> StepperChannel M0/M1
  -> MotionLock acquired
  -> HeaterScheduler forces HEATER_INHIBITED
  -> pull completes
  -> EVT,PULL telemetry event
```

## Manual-First Control

| Mode | Phase control | Heater control | Motor control |
|---|---|---|---|
| Connected Rev C | Operator commands `SET_PHASE` | Operator commands duties or PID targets; safety scheduler still applies | Operator jogs or runtime bend sequences |
| Link-loss fallback | Pressure FSM | Existing targets continue; untargeted channels use +5 C floor | Active sequence continues; non-sequence motion stops |

## Network Topology

| Port | Protocol | Direction | Purpose |
|---|---|---|---|
| `4000` | TCP | Pi -> laptop | Telemetry DATA and event frames |
| `5000` | TCP | Laptop -> Pi | One-shot command uplink |
| `4100` | UDP | Both | Discovery |

The Pi uses static link-local Ethernet `169.254.10.10/16`. The laptop may use
any `169.254.x.x/16` address. A successful command connection teaches the Pi
where telemetry should be returned.

## Final-BOM Hardware Boundaries

| Boundary | Configured in software | Physical-driver status |
|---|---|---|
| TMC2240 SPI setup | Yes | GPIO-CS SPI mode 3 register writes implemented; bench validation required |
| STEP/DIR/EN GPIO | Yes | libgpiod pulse backend implemented; bench timing validation required |
| Heater MOSFET outputs | Yes | zero-safe software PWM implemented; dummy-load validation required |
| DAQ132M Modbus | Yes | RTU/CRC/scaling implemented; exact register map must be verified |
| DPS310 I2C | Yes | compensated `i2c-dev` reads implemented |
| ADS1115 I2C | Yes | single-ended `i2c-dev` reads implemented |
| RTD Click | Optional config | Pins reserved; active MAX31865 read path not implemented |
| Resistance instrument | Disabled | Compatibility telemetry field only |

## Telemetry Shape

```text
DATA,<session>,<seq>,<timestamp>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>..<sample_7>,HEATER_DUTY=..,RESISTANCE=..,PHASE=..,MODE=..,STATUS=..,SENSOR_VALID=..,SENSOR_AGE_MS=..,COMPONENT_STATE=..,STEPPER0=..,STEPPER1=..
```

The ground station accepts the compatibility `RESISTANCE=` field, but the final
BOM has no resistance instrument, so normal final-BOM frames emit `-` values.
