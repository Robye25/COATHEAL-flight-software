# System Architecture

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
│  │  │  BME280 (I2C)│  │ Phase FSM    │  │  Sample PID (×9)         │  │   │
│  │  │  PT100s (SPI)│  │ Pressure     │  │  Box PID (×1)            │  │   │
│  │  │  ADS1115 (I2C│  │ transitions  │  └─────────────┬────────────┘  │   │
│  │  │  RTC (I2C)   │  └──────────────┘                │               │   │
│  │  └──────┬───────┘                        ┌──────────▼──────────┐   │   │
│  │         │ SensorSnapshot                 │  HeaterScheduler    │   │   │
│  │         │                                │  max 4 active / 40W │   │   │
│  │         └──────────────────────┐         └──────────┬──────────┘   │   │
│  │                                │                    │ duty[10]      │   │
│  │  ┌──────────────────────────┐  │         ┌──────────▼──────────┐   │   │
│  │  │   StorageManager         │  │         │ PwmController        │   │   │
│  │  │  SD: primary.csv         │◄─┤         │ (Simulated / GPIO)   │   │   │
│  │  │  USB: mirror.csv         │  │         └─────────────────────┘   │   │
│  │  └──────────────────────────┘  │                                   │   │
│  │                                │ TelemetryRecord                   │   │
│  │  ┌──────────────────────────┐  │                                   │   │
│  │  │   TelemetryQueue         │◄─┘                                   │   │
│  │  │  Disk-based durable FIFO │                                      │   │
│  │  └───────────┬──────────────┘                                      │   │
│  │              │ drain each tick                                      │   │
│  │  ┌───────────▼──────────────┐   ┌──────────────────────────────┐  │   │
│  │  │   TelemetryClient (TCP)  ├───►  CommandServer (TCP :5000)   │  │   │
│  │  │  :4000, ACK-based        │   │  Handles commands in thread  │  │   │
│  │  │  UDP discovery on :4100  │   └──────────────────────────────┘  │   │
│  │  └──────────────────────────┘                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                             │ TCP :4000 (telemetry)
                             │ TCP :5000 (commands)
                             │ UDP :4100 (discovery)
                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Ground Station (Windows/Linux PC)                     │
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                         gui_app.py (PyQt6)                         │    │
│  │                                                                    │    │
│  │  MainWindow (main thread)                                          │    │
│  │  ┌──────────────┐  ┌──────────────────┐  ┌──────────────────────┐ │    │
│  │  │ Control Dock │  │  Plot Tabs       │  │  Values Panel        │ │    │
│  │  │ Connection   │  │  (PyQtGraph)     │  │  (every telemetry    │ │    │
│  │  │ Heater Bars  │  │  Temperature     │  │   field live)        │ │    │
│  │  │ Commands     │  │  Pressure        │  └──────────────────────┘ │    │
│  │  └──────────────┘  │  Heater Duties   │                           │    │
│  │                    │  Environment     │  ┌──────────────────────┐ │    │
│  │                    └──────────────────┘  │  Log Dock            │ │    │
│  │                                          │  (timestamped events)│ │    │
│  │  ┌──────────────────────────────────┐    └──────────────────────┘ │    │
│  │  │ TelemetryReceiver (QThread)      │                             │    │
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

---

## Onboard Subsystems

### Main Loop (`SystemController::Run`)

Runs at `tick_hz` (default 1 Hz). Each tick:

1. Apply queued state/control overrides (from commands received between ticks)
2. Read sensor snapshot (SensorManager)
3. Update mission phase (StateManager)
4. Compute requested heater duties (ThermalController)
5. Schedule constrained duties (HeaterScheduler)
6. Apply duties to PWM hardware
7. Serialize telemetry record (Telemetry)
8. Write to both storage paths (StorageManager)
9. Enqueue telemetry frame (TelemetryQueue)
10. Drain queue to ground station (TelemetryClient)

### Command Handling

`CommandServer` runs a background thread listening on TCP port 5000. When a command arrives, it calls the handler registered by `SystemController::HandleCommandLine`. Safe commands (PING, STATUS, FORCE_START) take effect immediately or set a thread-safe override flag. The override is applied at the top of the next main loop tick.

### Telemetry Reliability

The onboard writes every frame to a durable disk queue before attempting to send. On each tick, `DrainTelemetryQueue` iterates all pending frames, sends each one, and waits for a per-frame ACK. Successfully ACK'd frames are removed. If the link is down, frames accumulate on disk (up to 72 hours / 8 GiB) and are replayed when the link is restored.

### Phase State Machine

```
            power-on
               │
               ▼
        ┌─────────────┐
        │ ASCENT_HOLD │  target: −30 °C
        └──────┬──────┘
               │ P < 140 mbar  OR  FORCE_START
               ▼
     ┌──────────────────┐
     │ ACTIVATION_RAMP  │  ramp: 0.85 °C/s → +70 °C
     └────────┬─────────┘
              │ ramp complete
              ▼
        ┌────────────┐
        │ FLOAT_HOLD │  target: +70 °C for 90 min
        └─────┬──────┘
              │ P > 300 mbar  OR  timeout
              ▼
      ┌───────────────┐
      │ DESCENT_FLOOR │  floor: −20 °C
      └───────┬───────┘
              │ float duration elapsed
              ▼
          ┌─────────┐
          │ STOPPED │
          └─────────┘

  FORCE_STOP / SHUTDOWN_SAFE → STOPPED (from any state)
  RESET_CTRL → resets thermal controller, stays in current phase
```

---

## Ground Station Threading

The GUI uses two threads to keep network I/O separate from the Qt event loop:

| Thread | Responsibility |
|---|---|
| **Main thread** | Qt event loop — all widget updates, plot redraws |
| **TelemetryReceiver (QThread)** | TCP server socket — `accept()`, `recv()`, parse, ACK, CSV write |

Data crosses the boundary via Qt signals:

- `packet_received(TelemetryPacket)` — delivered to main thread via queued connection; updates all plots and value panels
- `connection_changed(bool, str)` — updates connection label and status bar
- `log_message(str)` — appends to log panel

Commands are sent synchronously from the main thread (`CommandSender.send()`) as one-shot TCP connections. Because commands are infrequent and have a 3-second timeout, blocking the main thread briefly is acceptable.

---

## Network Topology

| Port | Protocol | Direction | Purpose |
|---|---|---|---|
| 4000 | TCP | Pi → Laptop | Telemetry stream (Pi connects out) |
| 5000 | TCP | Laptop → Pi | Command uplink (laptop connects in) |
| 4100 | UDP broadcast | Both | Discovery beacons |

The Pi acts as a TCP **client** for telemetry (it connects to the ground station). The Pi acts as a TCP **server** for commands (it listens on port 5000). This design means the ground station only needs one inbound firewall rule (port 4000) and one outbound connection (port 5000).

---

## Data Flow Summary

```
Sensors → SensorSnapshot → StateManager → MissionPhase
                       ↓
               ThermalController → duty[10]
                       ↓
               HeaterScheduler → constrained_duty[10]
                       ↓
               PwmController → GPIO/Simulated
                       ↓
               TelemetryRecord → serialize → frame string
                       ↓
               StorageManager (SD + USB CSV)
                       ↓
               TelemetryQueue (disk, durable)
                       ↓
               TelemetryClient → TCP → TelemetryReceiver
                                           ↓
                                       ACK → parse → CSV
                                           ↓
                                   Qt signals → GUI panels
```
