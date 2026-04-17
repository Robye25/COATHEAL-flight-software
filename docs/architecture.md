# System Architecture (Rev B.1)

## Overview

COATHEAL uses a client–server architecture over TCP/IP. The onboard C++ application runs autonomously on the Raspberry Pi 4 and streams telemetry to the ground station. The ground station receives, acknowledges, logs, and displays data, and sends operator commands back to the Pi.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Raspberry Pi 4 (Onboard)                           │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                       SystemController                              │   │
│  │                                                                     │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │   │
│  │  │SensorManager │  │ StateManager │  │    ThermalController     │  │   │
│  │  │ MS5803   I2C │  │ Phase FSM    │  │  Sample PID (×6)         │  │   │
│  │  │ PT100×8  RS485│ │ Pressure-    │  │  (no box PID)            │  │   │
│  │  │ ADS1015  I2C │  │ driven       │  └─────────────┬────────────┘  │   │
│  │  │ INA3221×2 I2C│  │ transitions  │                │               │   │
│  │  │ RTC      I2C │  └──────┬───────┘                │               │   │
│  │  └──────┬───────┘         │                ┌───────▼──────────┐    │   │
│  │         │ SensorSnapshot  │                │ HeaterScheduler  │    │   │
│  │         │ + resistance    │                │ 4 active / 20 W  │    │   │
│  │         │                 │                │ MotionLock gate  │    │   │
│  │         └─────────────────┤                └──────┬───────────┘    │   │
│  │                           │                       │ duty[6]        │   │
│  │  ┌──────────────────────┐ │                ┌──────▼──────────┐     │   │
│  │  │ StorageManager       │◄┤                │ PwmController   │     │   │
│  │  │  SD: primary.csv     │ │                │ (GPIO / sim)    │     │   │
│  │  │  USB: mirror.csv     │ │                └─────────────────┘     │   │
│  │  └──────────────────────┘ │                                        │   │
│  │                           │                ┌─────────────────┐     │   │
│  │  ┌──────────────────────┐ │                │ StepperChannel  │     │   │
│  │  │ TelemetryQueue       │◄┤                │  ×2 (TMC2240)   │     │   │
│  │  │ durable disk FIFO    │ │                │  MotionLock     │     │   │
│  │  └──────────┬───────────┘ │                │  EVT,PULL edge  │     │   │
│  │             │             │                └─────────────────┘     │   │
│  │  ┌──────────▼───────────┐ │                                        │   │
│  │  │ TelemetryClient (TCP)├─┤   ┌──────────────────────────────┐     │   │
│  │  │ :4000, ACK-based     │ │   │  CommandServer (TCP :5000)   │     │   │
│  │  │ UDP discovery :4100  │ │   │  Handles commands in thread  │     │   │
│  │  └──────────────────────┘ │   └──────────────────────────────┘     │   │
│  │                                                                     │   │
│  │  HAL adapters: SpiAdapter, I2cAdapter, RtcAdapter,                  │   │
│  │  Ina3221Adapter (sample-resistance science instrument, stub),       │   │
│  │  LibgpiodPwmController, GpioStatusLed, Rs485ModbusAdapter (planned) │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                             │ TCP :4000 (telemetry, DATA + EVT,PULL)
                             │ TCP :5000 (commands)
                             │ UDP :4100 (discovery)
                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Ground Station (Windows / Linux PC)                  │
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                         gui_app.py (PyQt6)                         │    │
│  │                                                                    │    │
│  │  MainWindow (main thread)                                          │    │
│  │  ┌──────────────┐  ┌──────────────────┐  ┌──────────────────────┐ │    │
│  │  │ Control Dock │  │  Plot Tabs       │  │  Values Panel        │ │    │
│  │  │ Connection   │  │  Sample temps    │  │  (every telemetry    │ │    │
│  │  │ Heater bars  │  │  Pressure        │  │   field live)        │ │    │
│  │  │ (6 × H0..H5) │  │  Heater duties   │  └──────────────────────┘ │    │
│  │  │ Commands     │  │  Resistance      │                           │    │
│  │  └──────────────┘  └──────────────────┘  ┌──────────────────────┐ │    │
│  │                                          │  Pull events dock    │ │    │
│  │  ┌──────────────────────────────────┐    │  (EVT,PULL table)    │ │    │
│  │  │ TelemetryReceiver (QThread)      │    └──────────────────────┘ │    │
│  │  │  TCP :4000 server                │                             │    │
│  │  │  Parse → ACK → CSV → signal      │                             │    │
│  │  └──────────────────────────────────┘                             │    │
│  │  ┌──────────────────────────────────┐                             │    │
│  │  │ CommandSender                    │                             │    │
│  │  │  One-shot TCP :5000              │                             │    │
│  │  └──────────────────────────────────┘                             │    │
│  └────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
```

The Rev B "Box PID (×1)" block is removed at Rev B.1: there is no electronics-box heater, no box temperature sensor, and no box PID. The ambient trace no longer includes humidity.

---

## Onboard subsystems

### Main loop (`SystemController::Run`)

Runs at `tick_hz` (default 1 Hz). Each tick:

1. Apply queued state/control overrides (from commands received between ticks).
2. Read sensor snapshot (`SensorManager`) — temperatures, pressure, UV, **sample resistance**.
3. Update mission phase (`StateManager`) — pressure-driven FSM.
4. Compute requested heater duties (`ThermalController`) — 6 per-sample PIDs.
5. Schedule constrained duties (`HeaterScheduler`) — 4-active / 20 W / 130 Wh / MotionLock gate.
6. Apply duties to PWM hardware (6 channels).
7. Tick both stepper channels (`StepperChannel::Tick`).
8. Build `TelemetryRecord` + status flags including `RESISTANCE_OK`.
9. Serialize DATA frame (`SerializeTelemetryDataFrame`) with `RESISTANCE=` segment.
10. Write to both storage paths (`StorageManager`).
11. Enqueue to `TelemetryQueue` and drain to ground (`TelemetryClient`).
12. Detect `moving`-edge falls per channel → emit `EVT,PULL,…`; feed `SensorManager::NotePullCompleted`.

### Command handling

`CommandServer` runs a background thread listening on TCP port 5000. On each connection it reads one line, passes it to `SystemController::HandleCommandLine`, writes the response, and closes. Safe commands take effect immediately or set a thread-safe override flag; the override is consumed at the top of the next main-loop tick.

### Telemetry reliability

Every frame (DATA and `EVT,PULL`) is written to the durable disk queue before being sent. On each tick, `DrainTelemetryQueue` iterates all pending frames, sends each one, and waits for a per-frame ACK. Successfully ACK'd frames are removed. If the link is down, frames accumulate on disk (up to 72 h / 8 GiB) and are replayed when the link is restored.

### Phase state machine (Rev B)

```
         power-on
            │
            ▼
       ┌─────────┐
       │  BOOT   │  heaters off (SystemMode = STANDBY / SAFE stays here)
       └────┬────┘
            │ SystemMode → RUN
            ▼
       ┌─────────┐
       │ ASCENT  │  floor +5 °C, per-sample PIDs with 0.5 °C hysteresis
       └────┬────┘
            │ P ≤ 100 mbar
            ▼
       ┌─────────┐
       │  FLOAT  │  pulls happen here (MotionLock + HEATER_INHIBITED)
       └────┬────┘
            │ P ≥ 300 mbar
            ▼
       ┌─────────┐
       │ DESCENT │  floor +5 °C
       └────┬────┘
            │ P ≥ 800 mbar
            ▼
       ┌─────────┐
       │ LANDED  │  heaters off
       └─────────┘

  FORCE_STOP / SHUTDOWN_SAFE → STOPPED (from any state)
  RESET_CTRL → resets per-sample PID integrators, stays in current phase
```

---

## Ground-station threading

| Thread | Responsibility |
|---|---|
| Main thread | Qt event loop — all widget updates, plot redraws |
| `TelemetryReceiver` (QThread) | TCP server socket — `accept()`, `recv()`, parse, ACK, CSV write |

Signals across the boundary (queued connection):

- `packet_received(TelemetryPacket)` → `_on_packet` — updates sample/heater/resistance plots and value panels.
- `pull_event(PullEvent)` → populates the Pull events table and appends to `<log>_pulls.csv`.
- `connection_changed(bool, str)` → status bar.
- `log_message(str)` → log panel.

Commands are sent synchronously from the main thread (`CommandSender.send()`) as one-shot TCP connections with a 3-second timeout.

---

## Network topology

| Port | Protocol | Direction | Purpose |
|---|---|---|---|
| 4000 | TCP | Pi → Laptop | Telemetry stream (Pi connects out) |
| 5000 | TCP | Laptop → Pi | Command uplink (laptop connects in) |
| 4100 | UDP broadcast | Both | Discovery beacons |

The Pi acts as a TCP **client** for telemetry and **server** for commands.

---

## Data-flow summary (Rev B.1)

```
Sensors → SensorSnapshot (temps, P, T, UV, resistance) → StateManager → MissionPhase
                     ↓
             ThermalController → duty[6]
                     ↓
             HeaterScheduler → constrained_duty[6]          MotionLock → (pull active?)
                     ↓                                              ↓
             PwmController (6 ch) → GPIO/Simulated     heater_inhibited → STATUS bit
                     ↓
             StepperChannel ×2 → TMC2240 → STEP/DIR/EN
                     ↓
             TelemetryRecord → SerializeTelemetryDataFrame
                     │   columns: temps(8), HEATER_DUTY=(6), RESISTANCE=(8),
                     │            PHASE, MODE, STATUS(13), STEPPER0, STEPPER1
                     ↓
             StorageManager (SD + USB CSV) ← also receives EVT,PULL strings
                     ↓
             TelemetryQueue (disk, durable)
                     ↓
             TelemetryClient → TCP → TelemetryReceiver
                                         ↓
                                     ACK → parse → CSV
                                         ↓
                                 Qt signals → GUI panels
```
