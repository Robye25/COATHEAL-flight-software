# COATHEAL Flight Software -- ESA Presentation

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Onboard Flight Software](#3-onboard-flight-software)
4. [Mission Phases & State Machine](#4-mission-phases--state-machine)
5. [Thermal Control System](#5-thermal-control-system)
6. [Communication Protocol](#6-communication-protocol)
7. [Ground Station](#7-ground-station)
8. [Reliability & Safety Design](#8-reliability--safety-design)
9. [Hardware Abstraction & Sensor Integration](#9-hardware-abstraction--sensor-integration)
10. [Build, Deployment & Testing](#10-build-deployment--testing)
11. [Configuration Reference](#11-configuration-reference)
12. [Key Parameters & Thresholds](#12-key-parameters--thresholds)

---

## 1. Project Overview

> **Rev A (historical) notice.** This presentation documents the Rev A thermal
> profile (ASCENT_HOLD −30 °C → ACTIVATION_RAMP → FLOAT_HOLD +70 °C →
> DESCENT_FLOOR −20 °C → STOPPED). Rev B collapsed this to a single
> cold-protection floor (`phase.sample_floor_c = +5 °C` shared across
> `ASCENT/FLOAT/DESCENT`) with a `BOOT → ASCENT → FLOAT → DESCENT → LANDED`
> FSM. Heater count dropped from 10 (9 samples + BOX) to 9 (8 samples + BOX).
> See `docs/configuration.md` and `docs/onboard.md` for the current design.

**COATHEAL** is a BEXUS high-altitude balloon experiment investigating thermal self-healing coatings. The flight software autonomously controls thermal profiles on material samples during all mission phases -- from ground through ascent, float at ~30 km altitude, and descent.

**Software Components:**

| Component | Language | Platform | Role |
|-----------|----------|----------|------|
| Onboard flight software | C++17 | Raspberry Pi 4 (aarch64, Debian) | Autonomous thermal control, telemetry, command handling |
| Ground station | Python 3 (PyQt6) | Any (Windows/Linux/macOS) | Real-time monitoring, command uplink, data logging |

**Communication:** TCP/IP + UDP discovery over Ethernet (E-Link or direct connection).

---

## 2. System Architecture

```
                         GROUND                                      ONBOARD (Raspberry Pi 4)
                    +------------------+                         +---------------------------+
                    |  Ground Station  |     TCP :4000           |    SystemController       |
                    |  (PyQt6 GUI)     |<---- Telemetry ------  |    (main loop @ 1 Hz)     |
                    |                  |                         |                           |
                    |  - Live plots    |---- ACK ------------->  |  +-- SensorManager        |
                    |  - Heater bars   |                         |  |   (PT100, BME280, RTC) |
                    |  - Command panel |     TCP :5000           |  |                        |
                    |  - CSV logging   |---- Commands -------->  |  +-- StateManager         |
                    |                  |                         |  |   (phase FSM)           |
                    |  - UDP discovery |     UDP :4100           |  |                        |
                    |    (GS_HELLO)    |<--- ONBOARD_HELLO ---   |  +-- ThermalController    |
                    +------------------+                         |  |   (dual PID)           |
                                                                 |  |                        |
                                                                 |  +-- HeaterScheduler      |
                                                                 |  |   (power budget)       |
                                                                 |  |                        |
                                                                 |  +-- PwmController        |
                                                                 |  |   (GPIO / simulated)   |
                                                                 |  |                        |
                                                                 |  +-- TelemetryClient      |
                                                                 |  |   (TCP uplink + queue) |
                                                                 |  |                        |
                                                                 |  +-- CommandServer         |
                                                                 |  |   (TCP :5000 listener) |
                                                                 |  |                        |
                                                                 |  +-- StorageManager        |
                                                                 |      (dual CSV: SD + USB) |
                                                                 +---------------------------+
```

**Data Flow per Tick (1 Hz):**

1. `SensorManager` reads all sensors (pressure, temperatures, humidity, UV)
2. `StateManager` evaluates phase transitions based on pressure thresholds
3. `ThermalController` computes heater duty cycles via dual PID controllers
4. `HeaterScheduler` enforces power constraints (max 4 active, max 40 W)
5. `PwmController` sets GPIO PWM outputs to heaters
6. Telemetry frame is built, written to dual CSV logs, and enqueued
7. `TelemetryClient` sends frames to ground station, awaits ACK

---

## 3. Onboard Flight Software

### 3.1 Entry Point

The onboard binary (`coatheal_onboard`) accepts a single argument:

```
coatheal_onboard --config <path-to-ini>
```

It loads the INI configuration, constructs the `SystemController`, calls `Initialize()`, then enters the main loop via `Run()`.

### 3.2 SystemController -- The Orchestrator

`SystemController` owns all subsystems and runs the deterministic main loop at a configurable rate (default 1 Hz). Each tick executes the full sense-decide-act pipeline described in Section 2.

**Thread Model:**

- **Main thread:** Runs the control loop. Owns all subsystem objects. No blocking I/O in the critical path.
- **Command server thread:** Listens on TCP port 5000. Communicates with the main thread via atomic override flags protected by `std::mutex`. Flags are read and cleared at the top of each tick to prevent mid-tick mutation.

**Override Flags (set by commands, consumed by main loop):**

| Flag | Effect |
|------|--------|
| `force_start` | Jump to ACTIVATION_RAMP |
| `force_stop` | Jump to DESCENT_FLOOR |
| `heaters_off` | All heater duties set to 0 |
| `reset_control` | Zero PID integrators |
| `shutdown_safe` | Graceful process exit |
| `single_heater_override` | Override one specific heater's duty |
| `all_heaters_override` | Override all heater duties to a single value |
| `pid_override` | Update sample PID gains (Kp, Ki, Kd) |

---

## 4. Mission Phases & State Machine

The `StateManager` implements a finite state machine with five phases. Transitions are triggered by pressure thresholds, temperature conditions, timers, or ground commands.

```
   ASCENT_HOLD            ACTIVATION_RAMP          FLOAT_HOLD           DESCENT_FLOOR          STOPPED
  (target -30 C)   -->   (ramp to +70 C)   -->   (hold +70 C)   -->   (floor -20 C)   -->   (heaters off)
                   P<=140                  T>=69 C              P>=300 mbar                shutdown
                   mbar                                          or 90 min
```

### Phase Details

| Phase | Setpoint | Trigger to Enter | Trigger to Exit |
|-------|----------|------------------|-----------------|
| **ASCENT_HOLD** | -30 C (sample) | System start | Pressure <= 140 mbar (~14 km altitude) |
| **ACTIVATION_RAMP** | Ramps from -30 C to +70 C at 0.85 C/s | Pressure threshold | All sample temps >= 69 C |
| **FLOAT_HOLD** | +70 C (sample) | Ramp complete | Pressure >= 300 mbar (descent) OR 90 min elapsed |
| **DESCENT_FLOOR** | -20 C (sample) | Descent detected | SHUTDOWN_SAFE command |
| **STOPPED** | 0 C (all off) | Command or timeout | Terminal state |

**Manual Overrides:**
- `FORCE_START` jumps directly to ACTIVATION_RAMP from any phase
- `FORCE_STOP` jumps directly to DESCENT_FLOOR from any phase

**Electronics Box:** Maintained at 0 C throughout all phases via a separate PID controller on heater index 9.

---

## 5. Thermal Control System

### 5.1 Dual PID Controllers

The `ThermalController` runs two independent PID controllers each tick:

| Controller | Sensors | Heaters | Setpoint Source |
|-----------|---------|---------|-----------------|
| **Sample PID** | 9 PT100 RTDs (average) | Heaters 0--8 | Current mission phase |
| **Box PID** | 1 box temperature sensor | Heater 9 | Fixed 0 C |

**PID Formula:**

```
output = Kp * error + Ki * integral + Kd * derivative
```

- **Integral anti-windup:** Integral term clamped to [-10, +10]
- **Output clamping:** Duty cycle clamped to [0.0, 1.0]
- **Derivative:** First-order hold on previous error

**Default Gains:**

| Gain | Sample PID | Box PID |
|------|-----------|---------|
| Kp | 0.20 | 0.15 |
| Ki | 0.02 | 0.01 |
| Kd | 0.03 | 0.02 |

PID gains can be overridden in-flight via the `SET_PID` debug command.

### 5.2 Heater Scheduler -- Power Budget Enforcement

The `HeaterScheduler` sits between the PID output and the PWM hardware. It enforces two hard constraints:

1. **Maximum 4 simultaneously active heaters** (hardware/power supply limit)
2. **Maximum 40 W aggregate thermal power** (10 W nominal per heater at 100% duty)

**Scheduling Algorithm:**

1. Clamp all requested duties to [0.0, 1.0]
2. Score each heater: `duty * weight` (electronics heater weighted 0.1x during ACTIVATION_RAMP to prioritize samples)
3. Rank by score descending; enable top N heaters (N = max_active_heaters)
4. If total estimated power exceeds 40 W, scale all duties down proportionally
5. Disabled heaters get duty = 0

### 5.3 PWM Output

- **Flight mode:** `LibgpiodPwmController` drives GPIO pins via `libgpiod`
- **Bench mode:** `SimulatedPwmController` stores duty values in memory (no hardware required)

---

## 6. Communication Protocol

### 6.1 Telemetry Downlink (TCP, port 4000)

The onboard software initiates an outbound TCP connection to the ground station. Each tick, it sends one telemetry frame as a CSV line:

```
DATA,<session_id>,<seq>,<timestamp>,<rtc_valid>,
     <ambient_temp_c>,<pressure_mbar>,<humidity_pct>,<uv>,<box_temp_c>,
     <sample_0>,...,<sample_8>,
     HEATER_DUTY=<d0>|<d1>|...|<d9>,
     PHASE=<phase_name>,
     STATUS=<flags>
```

**Example:**

```
DATA,coatheal-1718000000-123456,42,2024-06-10T12:00:00Z,1,-55.2,140.1,12.4,0.00012,-5.1,-30.1,-30.2,-30.0,-30.3,-30.1,-30.2,-30.0,-30.3,-30.1,HEATER_DUTY=0.25|0.00|0.25|0.00|0.00|0.00|0.00|0.00|0.00|0.00,PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK
```

**Status Flags:** `SD_OK`, `USB_OK`, `I2C_OK`, `SPI_OK`, `LINK_OK` -- each present only if healthy.

### 6.2 Acknowledgement (TCP, port 4000, ground to onboard)

```
ACK,<session_id>,<seq>
```

The ground station acknowledges each frame. The onboard telemetry queue uses the ACK to mark frames as delivered and remove them from the durable queue.

### 6.3 UDP Discovery (port 4100)

Automatic ground station discovery over broadcast:

| Direction | Message Format |
|-----------|---------------|
| Ground --> Broadcast | `GS_HELLO,<nonce>,<telemetry_port>,<command_port>` |
| Onboard --> Ground (unicast) | `ONBOARD_HELLO,<nonce>,<session_id>,<hostname>,<cmd_port>,<tel_port>` |

Falls back to static IP configuration (`comms.static_ground_ip`) if discovery is disabled or no response is received.

### 6.4 Command Uplink (TCP, port 5000)

The onboard command server accepts one client at a time on port 5000.

**Request/Response Format:**

```
Request:   <COMMAND> [<arg1> <arg2> ...]
Success:   ACK,<COMMAND>,<message>
Failure:   NACK,<COMMAND>,<reason>
```

### 6.5 Command Reference

**Safe Commands (always available):**

| Command | Arguments | Description |
|---------|-----------|-------------|
| `PING` | -- | Liveness check. Returns "pong". |
| `STATUS` | -- | Returns current phase, bench mode flag, queue depth. |
| `FORCE_START` | -- | Jump to ACTIVATION_RAMP phase. |
| `FORCE_STOP` | -- | Jump to DESCENT_FLOOR phase. |
| `HEATERS_OFF` | -- | Emergency: all heater duties to 0. |
| `RESET_CTRL` | -- | Reset PID integrators to zero. |
| `SHUTDOWN_SAFE` | -- | Graceful process shutdown. |

**Debug Commands (require `ARM_DEBUG <token>`, bench mode only):**

| Command | Arguments | Description |
|---------|-----------|-------------|
| `ARM_DEBUG` | `<token>` | Unlock debug commands. Token must match `runtime.debug_arm_code`. |
| `DISARM_DEBUG` | -- | Lock debug commands. |
| `SET_HEATER_DUTY` | `<index> <duty>` | Override single heater (0--9) to duty (0.0--1.0). |
| `SET_ALL_DUTY` | `<duty>` | Override all heaters to a single duty value. |
| `SET_PID` | `<kp> <ki> <kd>` | Override sample PID gains in real time. |
| `CLEAR_OVERRIDES` | -- | Clear all manual overrides, return to automatic control. |
| `SET_BENCH_MODE` | `0` or `1` | Toggle bench/simulation mode. |

**Aliases:** `ON` = FORCE_START, `OFF` = FORCE_STOP, `RESET` = RESET_CTRL.

---

## 7. Ground Station

### 7.1 Overview

The ground station is a Python application with two interfaces:

- **GUI mode** (`gui_app.py`): Full-featured PyQt6 dashboard with real-time plots, heater visualisation, and command panel
- **CLI mode** (`main.py`): Headless telemetry receiver and command client for scripted operations

### 7.2 GUI Layout

```
+------------------------------------------------------------------+
| COATHEAL Ground Station                                          |
+-------------------+----------------------------+-----------------+
|  CONNECTION       |                            |  LIVE VALUES    |
|  IP: [_________]  |  PLOT TABS                 |  Timestamp: ... |
|  Tel: [4000]      |  +----------------------+  |  Phase: ...     |
|  Cmd: [5000]      |  | Temperature         |  |  Pressure: ...  |
|  [Start/Stop]     |  | Pressure            |  |  Ambient T: ... |
|  Status: green    |  | Heater Duties       |  |  Box T: ...     |
|                   |  | Environment         |  |  Humidity: ...  |
|  HEATER DUTIES    |  +----------------------+  |  UV: ...        |
|  H0 [======  ] %  |                            |  Link: OK       |
|  H1 [==      ] %  |  (pyqtgraph real-time     |  Queue: 0       |
|  ...              |   scrolling plots,         |                 |
|  H8 [=====   ] %  |   1200-point window)       |                 |
|  BOX[===     ] %  |                            |                 |
|                   |                            |                 |
|  COMMANDS         |                            |                 |
|  [PING] [STATUS]  |                            |                 |
|  [FORCE_START]    |                            |                 |
|  [FORCE_STOP]     |                            |                 |
|  [HEATERS_OFF]    |                            |                 |
|  [SHUTDOWN_SAFE]  |                            |                 |
+-------------------+----------------------------+-----------------+
|  LOG OUTPUT                                                      |
|  > Connected from 169.254.10.10                                  |
|  > Received seq=42 ASCENT_HOLD                                   |
+------------------------------------------------------------------+
```

### 7.3 Plot Tabs

| Tab | Data Plotted | Y-axis |
|-----|-------------|--------|
| **Temperature** | Box temp + 9 sample temps vs. sequence | Degrees C |
| **Pressure** | Ambient pressure vs. sequence | mbar |
| **Heater Duties** | All 10 duty cycles vs. sequence | 0.0 -- 1.0 |
| **Environment** | Humidity (%) + UV (x100 scale) vs. sequence | % / scaled |

All plots use a rolling window of 1200 data points (~20 minutes at 1 Hz).

### 7.4 Threading Model

- **Main thread:** Qt event loop, all GUI widget updates
- **TelemetryReceiver (QThread):** TCP server on port 4000 -- accepts connections, parses frames, sends ACKs, writes CSV log, emits Qt signals to update the GUI

### 7.5 Data Logging

- Telemetry logged to `logs/ground_telemetry.csv`
- ACK cursor tracked in `logs/ground_ack_cursor.json` (deduplication across restarts)
- Session ID used to avoid replaying old frames

---

## 8. Reliability & Safety Design

### 8.1 Durable Telemetry Queue

The `TelemetryQueue` provides store-and-forward reliability:

- **Disk-backed:** Frames are persisted to `logs/telemetry-queue/pending.queue` as tab-separated lines
- **Survives crashes:** On restart, queued frames are reloaded and retransmitted
- **ACK-driven cleanup:** Only removes frames after the ground station acknowledges them
- **Retention policy:** Prunes frames older than 72 hours or exceeding 8 GiB total
- **Atomic writes:** Uses rename-based persistence to avoid corruption from partial writes

### 8.2 Dual CSV Logging

The `StorageManager` writes every telemetry frame to two independent paths:

- **Primary:** SD card (`logs/onboard_primary.csv`)
- **Secondary:** USB drive mirror (`logs/onboard_usb_mirror.csv`)

If one storage medium fails, the other continues independently. Health is reported in the `STATUS` field of telemetry frames (`SD_OK`, `USB_OK`).

### 8.3 Watchdog & Auto-Recovery

- **Systemd service** (`coatheal-onboard.service`) with `Restart=always`, `RestartSec=2`
- Rate-limited: max 10 restarts per 120-second window (`StartLimitBurst=10`)
- **Preflight healthcheck** runs before each start (`ExecStartPre`): validates config syntax and storage paths

### 8.4 Communication Resilience

- **ACK timeout:** 2-second window; if no ACK received, connection is closed and retried
- **Static IP fallback:** If UDP discovery fails, uses configured `comms.static_ground_ip`
- **Single-client command server:** Accepts only one command connection at a time (closes immediately after response) to prevent command interleaving

### 8.5 Command Safety

- **Dangerous commands** (FORCE_STOP, HEATERS_OFF, SHUTDOWN_SAFE) require confirmation dialogs in the ground station GUI
- **Debug commands** require explicit `ARM_DEBUG <token>` and bench mode enabled
- **HEATERS_OFF** is an emergency shutoff that overrides all PID output
- **CLEAR_OVERRIDES** returns the system to automatic PID control

### 8.6 Power Safety

- Hard limit of 4 simultaneously active heaters enforced in software
- Aggregate power capped at 40 W with proportional duty scaling
- Electronics heater de-prioritised during activation ramp to favour sample heating

---

## 9. Hardware Abstraction & Sensor Integration

### 9.1 HAL Architecture

All hardware access is abstracted through interface classes in `onboard/include/coatheal/hal/`:

| Adapter | Interface | Hardware | Status |
|---------|-----------|----------|--------|
| `I2cAdapter` | I2C | BME280 (ambient T/P/H), ADS1115 (UV ADC) | Stub (simulated in bench mode) |
| `SpiAdapter` | SPI | 9x MIKROE-2815 PT100 RTD modules | Stub (simulated in bench mode) |
| `RtcAdapter` | I2C | DS3231 real-time clock | Stub (uses system clock) |
| `PwmController` | GPIO | 10 heater channels via libgpiod | Implemented (simulated in bench mode) |

### 9.2 Bench Mode Simulation

When `runtime.bench_mode=true`, the `SensorManager` generates physically plausible simulated data:

- **Pressure:** Decreases at -1.5 mbar/s during ascent, increases at +1.8 mbar/s during descent
- **Ambient temperature:** -40 C (ascent), -55 C (activation/float), -15 C (descent)
- **Sample temperatures:** Converge toward PID setpoint with heating model (+5.0 C per W-s) and cooling model (0.03 * (T - T_ambient) * dt)
- **Box temperature:** Similar model with +2.5 C per W-s heating rate

This allows full end-to-end testing of the control loop, state machine, and telemetry without any hardware.

---

## 10. Build, Deployment & Testing

### 10.1 Build System

```bash
# Configure
cmake -S . -B build

# Build
cmake --build build -j$(nproc)

# Output binaries:
#   build/onboard/coatheal_onboard       (flight binary)
#   build/tests/coatheal_unit_tests       (test suite)
```

**Dependencies:** CMake >= 3.16, C++17 compiler, pthreads, libgpiod (optional, for flight GPIO).

### 10.2 Deployment to Raspberry Pi

```bash
# 1. Bootstrap Pi (installs build deps, enables I2C/SPI, creates directories)
./setup_coatheal.sh

# 2. Build on Pi
cmake -S . -B build && cmake --build build -j$(nproc)

# 3. Install systemd service
./scripts/install_onboard_service.sh /bexus/code/coatheal /bexus/code/coatheal/config/onboard.example.ini

# Service auto-starts on boot and restarts on crash
```

**Directory Layout on Pi:**

```
/bexus/
  code/coatheal/      # Repository root
    build/onboard/    # Compiled binary
    config/           # INI configuration
    scripts/          # Operational scripts
    logs/             # Runtime logs + telemetry queue
  data/               # Experiment data
  logs/               # System-level logs
  config/             # System-level config
```

### 10.3 Test Suite

The unit test suite (`tests/unit/test_suite.cpp`) covers:

| Test | What It Validates |
|------|------------------|
| `TestPidBoundsAndAntiWindup` | PID output clamping [0,1], integral anti-windup [-10,10] |
| `TestHeaterSchedulerCap` | Max 4 active heaters, power scaling under 40 W |
| `TestCommandParser` | Command parsing, argument validation, aliases (ON/OFF/RESET) |
| `TestTelemetrySerializer` | CSV frame format, field ordering, DATA prefix |
| `TestTelemetryQueuePersistenceAndAck` | Queue save/load to disk, ACK removes correct frames |
| `TestConfigParsesReliabilityFields` | INI parsing of all config sections |

```bash
# Run tests
ctest --test-dir build --output-on-failure
```

### 10.4 Ground Station Setup

```bash
cd ground-station
pip install -r requirements.txt    # PyQt6, pyqtgraph, numpy, matplotlib

# GUI mode (recommended)
python gui_app.py

# CLI mode
python main.py telemetry --port 4000
python main.py command --cmd "STATUS" --host 169.254.10.10
```

---

## 11. Configuration Reference

All parameters are set in a single INI file. See `config/onboard.example.ini`.

### Runtime

| Key | Default | Description |
|-----|---------|-------------|
| `runtime.tick_hz` | 1.0 | Main loop frequency (Hz) |
| `runtime.bench_mode` | false | Enable simulation mode |
| `runtime.debug_arm_code` | COATHEAL_DEBUG | Token for ARM_DEBUG command |
| `runtime.use_simulated_pwm` | false | Use simulated PWM (auto-true in bench mode) |
| `runtime.gpio_chip` | /dev/gpiochip0 | GPIO device path |

### Communications

| Key | Default | Description |
|-----|---------|-------------|
| `comms.telemetry_host` | 192.168.50.1 | Ground station IP for telemetry |
| `comms.telemetry_port` | 4000 | Telemetry TCP port |
| `comms.command_port` | 5000 | Command TCP listen port |
| `comms.reconnect_ms` | 2000 | ACK timeout / reconnect window |
| `comms.discovery_enabled` | true | Enable UDP discovery |
| `comms.discovery_port` | 4100 | UDP discovery port |

### Thermal Phases

| Key | Default | Description |
|-----|---------|-------------|
| `phase.ascent_target_c` | -30.0 | Ascent hold setpoint |
| `phase.activation_target_c` | 70.0 | Activation ramp target |
| `phase.float_target_c` | 70.0 | Float hold setpoint |
| `phase.descent_floor_c` | -20.0 | Descent floor setpoint |
| `phase.box_target_c` | 0.0 | Electronics box setpoint |
| `phase.activation_ramp_c_per_s` | 0.85 | Ramp rate (C/s) |
| `phase.float_hold_minutes` | 90.0 | Float duration before descent |

### Pressure Transitions

| Key | Default | Description |
|-----|---------|-------------|
| `transition.ascent_to_activation_mbar` | 140.0 | Ascent to activation trigger |
| `transition.float_to_descent_mbar` | 300.0 | Float to descent trigger |

### Power Budget

| Key | Default | Description |
|-----|---------|-------------|
| `power.max_active_heaters` | 4 | Max simultaneous heaters |
| `power.max_thermal_w` | 40.0 | Max aggregate thermal power |
| `power.heater_nominal_w` | 10.0 | Power per heater at 100% duty |

### PID Gains

| Key | Default | Description |
|-----|---------|-------------|
| `pid.kp` / `pid.ki` / `pid.kd` | 0.20 / 0.02 / 0.03 | Sample PID gains |
| `pid.box_kp` / `pid.box_ki` / `pid.box_kd` | 0.15 / 0.01 / 0.02 | Box PID gains |

---

## 12. Key Parameters & Thresholds

| Parameter | Value | Significance |
|-----------|-------|-------------|
| Main loop rate | 1 Hz | One telemetry frame per second |
| Ascent hold setpoint | -30 C | Maintain samples during climb |
| Activation target | +70 C | Self-healing coating activation temperature |
| Ramp rate | 0.85 C/s | Controlled heating to avoid thermal shock |
| Float hold duration | 90 minutes | Observation window at altitude |
| Descent floor | -20 C | Prevent excessive cooling on return |
| Ascent-to-activation pressure | 140 mbar | Approximately 14 km altitude |
| Float-to-descent pressure | 300 mbar | Approximately 9 km altitude (descending) |
| Max active heaters | 4 of 10 | Hardware power supply constraint |
| Max thermal power | 40 W | Aggregate power budget |
| Heater nominal power | 10 W each | At 100% duty cycle |
| Total system power budget | 48.23 W | Including computing + comms |
| Telemetry ACK timeout | 2 seconds | Reconnect if no acknowledgement |
| Queue retention | 72 hours | Store unsent frames on disk |
| Queue max size | 8 GiB | Disk space limit for queue |

---

*Document prepared for ESA BEXUS programme review.*
