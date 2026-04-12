# COATHEAL Software -- Presentation Notes

## 1. Introduction

- COATHEAL = BEXUS balloon experiment, thermal self-healing coatings
- Software job: autonomously control sample temperature through entire flight
- Two components:
  - Onboard: C++17 on Raspberry Pi 4 -- thermal control, telemetry, commands
  - Ground station: Python (PyQt6) -- live monitoring, command uplink, logging

---

## 2. Architecture

- Central component: **SystemController**, runs main loop at **1 Hz**
- Each tick (once per second):
  1. Read sensors (pressure, 9 sample temps, box temp, humidity, UV)
  2. Evaluate mission phase (pressure-based state machine)
  3. Compute heater duties (dual PID controllers)
  4. Enforce power budget (max 4 heaters, max 40 W)
  5. Set PWM outputs to heaters
  6. Log to SD + USB, send telemetry to ground
- Two threads:
  - Main thread: control loop (no blocking I/O)
  - Command thread: TCP listener on port 5000, sets override flags

---

## 3. Mission Phases (State Machine)

```
ASCENT_HOLD --> ACTIVATION_RAMP --> FLOAT_HOLD --> DESCENT_FLOOR --> STOPPED
```

| Phase | Setpoint | Entry trigger | Exit trigger |
|-------|----------|---------------|--------------|
| Ascent Hold | -30 C | System start | Pressure <= 140 mbar (~14 km) |
| Activation Ramp | -30 C to +70 C, 0.85 C/s | Pressure threshold | Samples reach 69 C |
| Float Hold | +70 C | Ramp complete | Pressure >= 300 mbar OR 90 min |
| Descent Floor | -20 C floor | Descent detected | Shutdown command |
| Stopped | Off | Command/timeout | Terminal |

- Manual overrides: FORCE_START (jump to ramp), FORCE_STOP (jump to descent)
- Electronics box: separate PID, held at 0 C throughout

---

## 4. Thermal Control

- **Two independent PID controllers:**
  - Sample PID: average of 9 PT100 readings --> heaters 0-8
  - Box PID: box sensor --> heater 9
- PID output: duty cycle 0.0 to 1.0
- Anti-windup: integral clamped [-10, +10]
- Default gains: Kp=0.20, Ki=0.02, Kd=0.03 (sample); adjustable in-flight

- **Heater Scheduler (power enforcement):**
  - Max 4 active heaters simultaneously
  - Max 40 W total (10 W per heater at 100%)
  - Ranks heaters by requested duty, enables top 4
  - If over budget: scale all duties down proportionally
  - During ramp: electronics heater de-prioritised (0.1x weight)

---

## 5. Communication

- **Telemetry downlink** -- TCP port 4000, 1 frame/sec
  - Format: CSV line with session ID, sequence number, timestamp, all sensor values, heater duties, phase, status flags
  - Ground replies with ACK per frame

- **Command uplink** -- TCP port 5000
  - Safe commands: PING, STATUS, FORCE_START, FORCE_STOP, HEATERS_OFF, RESET_CTRL, SHUTDOWN_SAFE
  - Debug commands (bench mode only, require arming token): SET_HEATER_DUTY, SET_ALL_DUTY, SET_PID, CLEAR_OVERRIDES
  - Response: ACK or NACK

- **UDP discovery** -- port 4100
  - Ground broadcasts GS_HELLO, onboard replies ONBOARD_HELLO
  - Fallback: static IP from config file

---

## 6. Ground Station

- PyQt6 + PyQtGraph GUI
- Left panel: connection settings, 10 heater duty bars (live), command buttons
- Centre: tabbed plots -- temperature, pressure, heater duties, environment
- Right: live values readout (all fields)
- Background thread receives telemetry, sends ACKs, logs CSV
- Dangerous commands require confirmation dialog

---

## 7. Reliability & Safety

- **Durable telemetry queue:**
  - Every frame saved to disk before sending
  - Survives crashes/restarts -- reloads and retransmits
  - Frames removed only after ground ACKs
  - Retention: 72 hours / 8 GiB max

- **Dual logging:** SD card + USB mirror, independent -- one can fail

- **Auto-restart:** systemd service, restarts in 2 sec on crash, preflight healthcheck before each start

- **Communication resilience:** 2-sec ACK timeout then reconnect, static IP fallback

- **Command safety:** dangerous commands need GUI confirmation, debug commands need arming + bench mode

- **Power safety:** 4-heater limit + 40 W cap enforced every tick, HEATERS_OFF for emergency shutoff

- **Runaway protection (3 layers):**
  1. PID output clamped [0, 1]
  2. Scheduler enforces 40 W / 4 heater cap
  3. Ground can send HEATERS_OFF at any time

---

## 8. Hardware Abstraction & Bench Mode

- All hardware behind abstraction layer (I2C, SPI, RTC, PWM adapters)
- **Bench mode** = full simulation, no hardware needed:
  - Pressure: decreases during ascent, increases during descent
  - Sample temps: respond to heater input (thermal model)
  - Complete state machine + control loop runs identically to flight
- Real sensor drivers (BME280, PT100, DS3231) to be integrated before flight
- PWM: simulated in bench mode, libgpiod GPIO in flight

---

## 9. Build & Testing

- CMake build, single binary: `coatheal_onboard --config <ini-file>`
- All parameters in one INI file (no recompile to adjust)
- Test suite covers: PID bounds, heater scheduler cap, command parsing, telemetry format, queue persistence, config parsing
- Deployment: bootstrap script on Pi, cmake build, systemd service install

---

## 10. Summary Points

- Fully autonomous thermal control -- works without ground link
- Deterministic 1 Hz loop, dual PID, power-constrained scheduling
- Reliable telemetry: ACK-based with durable disk queue
- Resilient: dual logging, auto-restart, multiple safety layers
- Entire stack testable in simulation (bench mode)
- Total power budget: 48.23 W system, 40 W thermal

---

## Q&A Cheat Sheet

| Question | Key points |
|----------|-----------|
| Link loss during ramp? | Fully autonomous, continues ramp/float/descent on its own. Queue stores frames, retransmits on reconnect. |
| Heater/sensor failure? | Scheduler adapts to fewer heaters. 9 sensors averaged -- one outlier shifts average slightly, PID clamping prevents dangerous response. |
| Override phase transitions? | FORCE_START (jump to ramp), FORCE_STOP (jump to descent), from any phase. |
| Power budget? | 48.23 W total, 40 W thermal max, 10 W per heater, max 4 simultaneous. |
| Runaway heating prevention? | PID clamp + scheduler cap + HEATERS_OFF command. Three independent layers. |
| Why C++ not Python onboard? | Deterministic 1 Hz timing, low resource usage, precise thread/memory control. |
| Testing without balloon? | Bench mode: simulated sensors with thermal model, full end-to-end test on laptop. |
